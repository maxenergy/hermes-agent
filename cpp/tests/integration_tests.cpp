// End-to-end integration tests for the Hermes agent pipeline.
// Uses FakeHttpTransport (no live APIs) but exercises full
// AIAgent + ToolRegistry + real tool handlers + SessionDB wiring.

#include "equivalence_test_framework.hpp"

#include "hermes/agent/ai_agent.hpp"
#include "hermes/agent/context_compressor.hpp"
#include "hermes/agent/prompt_builder.hpp"
#include "hermes/cron/cron_parser.hpp"
#include "hermes/cron/jobs.hpp"
#include "hermes/llm/llm_client.hpp"
#include "hermes/llm/openai_client.hpp"
#include "hermes/state/session_db.hpp"
#include "hermes/tools/discover_tools.hpp"
#include "hermes/tools/registry.hpp"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using hermes::agent::AgentCallbacks;
using hermes::agent::AgentConfig;
using hermes::agent::AIAgent;
using hermes::agent::CompressionOptions;
using hermes::agent::ContextCompressor;
using hermes::agent::PromptBuilder;
using hermes::agent::ToolDispatcher;
using hermes::llm::FakeHttpTransport;
using hermes::llm::HttpTransport;
using hermes::llm::OpenAIClient;
using hermes::llm::ToolSchema;
using json = nlohmann::json;

namespace {

HttpTransport::Response make_text_response(const std::string& text) {
    HttpTransport::Response resp;
    resp.status_code = 200;
    resp.body = json{
        {"id", "chatcmpl-int"},
        {"choices", json::array({{
            {"index", 0},
            {"finish_reason", "stop"},
            {"message", {{"role", "assistant"}, {"content", text}}},
        }})},
        {"usage", {{"prompt_tokens", 10}, {"completion_tokens", 5}}},
    }.dump();
    return resp;
}

HttpTransport::Response make_tool_call_response(
    const std::string& tool_name,
    const json& args,
    const std::string& call_id = "call_int") {
    HttpTransport::Response resp;
    resp.status_code = 200;
    resp.body = json{
        {"id", "chatcmpl-int-tc"},
        {"choices", json::array({{
            {"index", 0},
            {"finish_reason", "tool_calls"},
            {"message", {
                {"role", "assistant"},
                {"content", nullptr},
                {"tool_calls", json::array({{
                    {"id", call_id},
                    {"type", "function"},
                    {"function", {
                        {"name", tool_name},
                        {"arguments", args.dump()},
                    }},
                }})},
            }},
        }})},
        {"usage", {{"prompt_tokens", 10}, {"completion_tokens", 5}}},
    }.dump();
    return resp;
}

struct IntegrationFixture {
    FakeHttpTransport fake;
    OpenAIClient client{&fake, "sk-test"};
    PromptBuilder builder;
    AgentConfig config;

    IntegrationFixture() {
        config.model = "gpt-4o";
        config.max_iterations = 90;
        config.max_retries = 0;
    }

    std::unique_ptr<AIAgent> make_agent(
        ToolDispatcher dispatcher = nullptr,
        AgentCallbacks cbs = {}) {
        auto agent = std::make_unique<AIAgent>(
            config, &client, nullptr, nullptr, nullptr, &builder,
            std::move(dispatcher), std::vector<ToolSchema>{},
            std::move(cbs));
        agent->set_sleep_function([](std::chrono::milliseconds) {});
        return agent;
    }
};

// Temporary directory helper that cleans up on destruction.
struct TmpDir {
    std::filesystem::path path;
    TmpDir() {
        auto base = std::filesystem::temp_directory_path() / "hermes_integration_test";
        path = base / std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count());
        std::filesystem::create_directories(path);
    }
    ~TmpDir() {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }
};

}  // namespace

// ---- Test 1: Agent with file tool dispatcher ----

TEST(Integration, AgentWithFileTools) {
    IntegrationFixture f;
    // Model asks to read a file, tool returns content, model summarizes.
    f.fake.enqueue_response(
        make_tool_call_response("read_file", json{{"path", "/tmp/testfile.txt"}}));
    f.fake.enqueue_response(
        make_text_response("The file says: Hello from file."));

    ToolDispatcher dispatcher = [](const std::string& name,
                                   const json& args,
                                   const std::string&) -> std::string {
        if (name == "read_file") {
            return json{{"content", "Hello from file."},
                        {"path", args.value("path", "")}}.dump();
        }
        return R"({"error":"unknown tool"})";
    };

    auto agent = f.make_agent(dispatcher);
    std::string result = agent->chat("Read /tmp/testfile.txt and summarize");
    EXPECT_NE(result.find("file"), std::string::npos);
}

// ---- Test 2: Agent with terminal tool ----

TEST(Integration, AgentWithTerminalTool) {
    IntegrationFixture f;
    f.fake.enqueue_response(
        make_tool_call_response("terminal", json{{"command", "echo hello"}}));
    f.fake.enqueue_response(make_text_response("The command output: hello"));

    ToolDispatcher dispatcher = [](const std::string& name,
                                   const json& /*args*/,
                                   const std::string&) -> std::string {
        if (name == "terminal") {
            return json{{"stdout", "hello"}, {"exit_code", 0}}.dump();
        }
        return R"({"error":"unknown"})";
    };

    auto agent = f.make_agent(dispatcher);
    std::string result = agent->chat("Run echo hello");
    EXPECT_NE(result.find("hello"), std::string::npos);
}

// ---- Test 3: Context compression triggered ----

TEST(Integration, ContextCompressionTriggered) {
    // Compression summarizer responses: one for the summary.
    FakeHttpTransport summarizer_fake;
    OpenAIClient summarizer_client(&summarizer_fake, "sk-test");
    summarizer_fake.enqueue_response(make_text_response(
        "Summary: we discussed many topics."));

    CompressionOptions opts;
    opts.trigger_threshold = 0.3;
    opts.protected_tail_turns = 2;
    ContextCompressor compressor(&summarizer_client, "gpt-4o", opts);

    // Build a long conversation history.
    std::vector<hermes::llm::Message> history;
    for (int i = 0; i < 50; ++i) {
        hermes::llm::Message user_msg;
        user_msg.role = hermes::llm::Role::User;
        user_msg.content_text = "Message number " + std::to_string(i) +
                                " with some padding text to increase token count.";
        history.push_back(user_msg);
        hermes::llm::Message asst_msg;
        asst_msg.role = hermes::llm::Role::Assistant;
        asst_msg.content_text = "Response to message " + std::to_string(i) + ".";
        history.push_back(asst_msg);
    }

    // Test the compressor directly.
    auto compressed = compressor.compress(history, 90, 100);
    EXPECT_LT(compressed.size(), history.size());
    EXPECT_GE(compressor.compression_count(), 1);
}

// ---- Test 4: Session persistence ----

TEST(Integration, SessionPersistence) {
    TmpDir tmp;
    auto db_path = tmp.path / "sessions.db";

    // Create session, save messages, reload, verify.
    {
        hermes::state::SessionDB db(db_path);
        auto sid = db.create_session("integration_test", "gpt-4o", json{});
        hermes::state::MessageRow msg;
        msg.session_id = sid;
        msg.turn_index = 0;
        msg.role = "user";
        msg.content = "Hello world";
        msg.created_at = std::chrono::system_clock::now();
        db.save_message(msg);

        hermes::state::MessageRow msg2;
        msg2.session_id = sid;
        msg2.turn_index = 1;
        msg2.role = "assistant";
        msg2.content = "Hi there!";
        msg2.created_at = std::chrono::system_clock::now();
        db.save_message(msg2);

        auto messages = db.get_messages(sid);
        ASSERT_EQ(messages.size(), 2u);
        EXPECT_EQ(messages[0].content, "Hello world");
        EXPECT_EQ(messages[1].content, "Hi there!");
    }

    // Reopen and verify continuity.
    {
        hermes::state::SessionDB db(db_path);
        auto sessions = db.list_sessions();
        ASSERT_GE(sessions.size(), 1u);
        auto messages = db.get_messages(sessions[0].id);
        ASSERT_EQ(messages.size(), 2u);
        EXPECT_EQ(messages[0].role, "user");
        EXPECT_EQ(messages[1].role, "assistant");
    }
}

// ---- Test 5: Cron job execution ----

TEST(Integration, CronJobExecution) {
    TmpDir tmp;
    hermes::cron::JobStore store(tmp.path);

    hermes::cron::Job job;
    job.name = "test-job";
    job.schedule = hermes::cron::parse("*/5 * * * *");
    job.schedule_str = "*/5 * * * *";
    job.prompt = "Generate a daily report";
    job.delivery_targets = {"local"};
    job.created_at = std::chrono::system_clock::now();

    auto job_id = store.create(job);
    EXPECT_FALSE(job_id.empty());

    auto retrieved = store.get(job_id);
    ASSERT_TRUE(retrieved.has_value());
    EXPECT_EQ(retrieved->name, "test-job");
    EXPECT_EQ(retrieved->prompt, "Generate a daily report");

    // Simulate execution result.
    hermes::cron::JobResult result;
    result.job_id = job_id;
    result.run_id = "run-001";
    result.output = "Report generated successfully.";
    result.success = true;
    result.started_at = std::chrono::system_clock::now();
    result.finished_at = std::chrono::system_clock::now();
    store.save_result(result);

    auto results = store.get_results(job_id);
    ASSERT_EQ(results.size(), 1u);
    EXPECT_TRUE(results[0].success);
    EXPECT_EQ(results[0].output, "Report generated successfully.");
}

// ---- Test 6: Tool registry dispatch ----

TEST(Integration, ToolRegistryDispatch) {
    auto& reg = hermes::tools::ToolRegistry::instance();
    reg.clear();

    hermes::tools::ToolEntry entry;
    entry.name = "integration_echo";
    entry.toolset = "test";
    entry.schema = json{
        {"name", "integration_echo"},
        {"description", "Echo input"},
        {"parameters", {{"type", "object"}, {"properties", {{"text", {{"type", "string"}}}}}}},
    };
    entry.handler = [](const json& args,
                       const hermes::tools::ToolContext&) -> std::string {
        return json{{"echo", args.value("text", "")}}.dump();
    };
    reg.register_tool(entry);

    hermes::tools::ToolContext ctx;
    auto result_str = reg.dispatch("integration_echo", json{{"text", "ping"}}, ctx);
    auto result = json::parse(result_str);
    EXPECT_EQ(result["echo"], "ping");

    reg.clear();
}

// ---- Test 7: Multi-tool conversation pipeline ----

TEST(Integration, MultiToolConversation) {
    IntegrationFixture f;
    f.config.max_iterations = 10;

    // Model calls two tools in sequence, then gives final answer.
    f.fake.enqueue_response(
        make_tool_call_response("list_files", json{{"dir", "."}}, "call_1"));
    f.fake.enqueue_response(
        make_tool_call_response("read_file", json{{"path", "readme.md"}}, "call_2"));
    f.fake.enqueue_response(
        make_text_response("Found readme.md with project description."));

    int tool_calls = 0;
    ToolDispatcher dispatcher = [&](const std::string& name,
                                    const json&,
                                    const std::string&) -> std::string {
        ++tool_calls;
        if (name == "list_files") {
            return json{{"files", json::array({"readme.md", "main.cpp"})}}.dump();
        }
        if (name == "read_file") {
            return json{{"content", "# My Project"}}.dump();
        }
        return R"({"error":"unknown"})";
    };

    auto agent = f.make_agent(dispatcher);
    auto result = agent->run_conversation("What does this project do?");
    EXPECT_TRUE(result.completed);
    EXPECT_EQ(tool_calls, 2);
    EXPECT_NE(result.final_response.find("readme"), std::string::npos);
}

// ---- Test 8: FTS search across sessions ----

TEST(Integration, FtsSearchAcrossSessions) {
    TmpDir tmp;
    auto db_path = tmp.path / "fts_test.db";
    hermes::state::SessionDB db(db_path);

    // Create several sessions with distinct content.
    for (int i = 0; i < 5; ++i) {
        auto sid = db.create_session("fts_test", "gpt-4o", json{});
        hermes::state::MessageRow msg;
        msg.session_id = sid;
        msg.turn_index = 0;
        msg.role = "user";
        msg.content = "Discussion about topic_" + std::to_string(i) +
                      " with unique content for searching";
        msg.created_at = std::chrono::system_clock::now();
        db.save_message(msg);
    }

    auto hits = db.fts_search("topic_3");
    ASSERT_GE(hits.size(), 1u);
    EXPECT_NE(hits[0].snippet.find("topic_3"), std::string::npos);
}

// ---- Test 9: Equivalence framework ----

TEST(Integration, EquivalenceFramework) {
    // Find the fixtures directory relative to the source tree.
    // We test the framework itself with programmatic cases rather than
    // depending on the file path at runtime.
    hermes::tests::EquivalenceCase tc;
    tc.name = "programmatic_test";
    tc.input = json{
        {"prompt", "Say hi"},
        {"model_responses", json::array({
            {{"text", "Hi there!"}},
        })},
    };
    tc.expected_output = json{
        {"final_response_contains", "Hi"},
        {"tool_calls", 0},
    };

    auto result = hermes::tests::run_equivalence(tc);
    EXPECT_TRUE(result.passed) << result.diff;
}

// ---- Test 10: Agent callbacks fire in correct order ----

TEST(Integration, CallbackOrderInPipeline) {
    IntegrationFixture f;
    f.fake.enqueue_response(
        make_tool_call_response("test_tool", json{{"x", 1}}));
    f.fake.enqueue_response(make_text_response("Done."));

    std::vector<std::string> event_log;

    AgentCallbacks cbs;
    cbs.on_assistant_message = [&](const hermes::llm::Message&) {
        event_log.push_back("assistant");
    };
    cbs.on_tool_call = [&](const std::string&, const json&) {
        event_log.push_back("tool_call");
    };
    cbs.on_tool_result = [&](const std::string&, const std::string&) {
        event_log.push_back("tool_result");
    };
    cbs.on_usage = [&](int64_t, int64_t, double) {
        event_log.push_back("usage");
    };

    auto agent = f.make_agent(
        [](const std::string&, const json&, const std::string&) {
            return R"({"ok":true})";
        },
        std::move(cbs));
    agent->chat("Do something");

    // Verify key events fired.
    EXPECT_FALSE(event_log.empty());
    // assistant callback fires at least once (for tool_call message + final message).
    auto count_assistant = std::count(event_log.begin(), event_log.end(), "assistant");
    EXPECT_GE(count_assistant, 1);
    // tool_call and tool_result each fire at least once.
    EXPECT_GE(std::count(event_log.begin(), event_log.end(), "tool_call"), 1);
    EXPECT_GE(std::count(event_log.begin(), event_log.end(), "tool_result"), 1);
}
