#include "hermes/agent/ai_agent.hpp"

#include "hermes/llm/llm_client.hpp"
#include "hermes/llm/openai_client.hpp"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <string>
#include <vector>

using hermes::agent::AgentCallbacks;
using hermes::agent::AgentConfig;
using hermes::agent::AIAgent;
using hermes::agent::ConversationResult;
using hermes::agent::PromptBuilder;
using hermes::agent::PromptContext;
using hermes::agent::ToolDispatcher;
using hermes::llm::FakeHttpTransport;
using hermes::llm::HttpTransport;
using hermes::llm::OpenAIClient;
using json = nlohmann::json;

namespace {

// Helper: create a 200 OpenAI-style text response.
HttpTransport::Response make_text_response(const std::string& text) {
    HttpTransport::Response resp;
    resp.status_code = 200;
    resp.body = json{
        {"id", "chatcmpl-1"},
        {"choices", json::array({{
            {"index", 0},
            {"finish_reason", "stop"},
            {"message", {
                {"role", "assistant"},
                {"content", text},
            }},
        }})},
        {"usage", {{"prompt_tokens", 10}, {"completion_tokens", 5}}},
    }.dump();
    return resp;
}

// Helper: create a 200 OpenAI-style tool_call response.
HttpTransport::Response make_tool_call_response(
    const std::string& tool_name,
    const json& args,
    const std::string& call_id = "call_1") {
    HttpTransport::Response resp;
    resp.status_code = 200;
    resp.body = json{
        {"id", "chatcmpl-tc"},
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

struct AgentFixture {
    FakeHttpTransport fake;
    OpenAIClient client{&fake, "sk-test"};
    PromptBuilder builder;
    AgentConfig config;

    AgentFixture() {
        config.model = "gpt-4o";
        config.max_iterations = 90;
        config.max_retries = 0;
    }

    std::unique_ptr<AIAgent> make_agent(
        ToolDispatcher dispatcher = nullptr,
        AgentCallbacks cbs = {}) {
        auto agent = std::make_unique<AIAgent>(
            config, &client, nullptr, nullptr, nullptr, &builder,
            std::move(dispatcher), std::vector<hermes::llm::ToolSchema>{},
            std::move(cbs));
        agent->set_sleep_function([](std::chrono::milliseconds) {});
        return agent;
    }
};

}  // namespace

TEST(AIAgent, HappyPath) {
    AgentFixture f;
    f.fake.enqueue_response(make_text_response("Hello, world!"));
    auto agent = f.make_agent();
    std::string result = agent->chat("Hi");
    EXPECT_EQ(result, "Hello, world!");
}

TEST(AIAgent, ToolCallPath) {
    AgentFixture f;
    // First response: model calls a tool.
    f.fake.enqueue_response(
        make_tool_call_response("read_file", json{{"path", "/tmp/a"}}));
    // Second response: model returns final text after tool result.
    f.fake.enqueue_response(make_text_response("File contents shown."));

    bool dispatcher_called = false;
    ToolDispatcher dispatcher = [&](const std::string& name,
                                    const json& args,
                                    const std::string&) -> std::string {
        dispatcher_called = true;
        EXPECT_EQ(name, "read_file");
        EXPECT_EQ(args["path"], "/tmp/a");
        return R"({"ok":true})";
    };

    auto agent = f.make_agent(dispatcher);
    std::string result = agent->chat("Read file");
    EXPECT_TRUE(dispatcher_called);
    EXPECT_EQ(result, "File contents shown.");
}

TEST(AIAgent, IterationCap) {
    AgentFixture f;
    f.config.max_iterations = 1;
    // Model keeps calling tools — only 1 iteration allowed.
    f.fake.enqueue_response(
        make_tool_call_response("read_file", json{{"path", "/a"}}));
    // After 1 iteration the loop should stop — but the agent will still try
    // one more LLM call in the while condition check. We give it a tool_call
    // just in case.
    f.fake.enqueue_response(
        make_tool_call_response("read_file", json{{"path", "/b"}}));

    auto agent = f.make_agent([](const std::string&, const json&,
                                 const std::string&) {
        return R"({"ok":true})";
    });

    auto result = agent->run_conversation("Do something");
    EXPECT_FALSE(result.completed);
}

TEST(AIAgent, StopRequest) {
    AgentFixture f;
    auto agent = f.make_agent();
    agent->request_stop();
    auto result = agent->run_conversation("Hi");
    EXPECT_FALSE(result.completed);
    EXPECT_TRUE(result.error.has_value());
}

TEST(AIAgent, TodoIntercept) {
    AgentFixture f;
    // Model calls the "todo" tool — should be intercepted by agent, not dispatcher.
    f.fake.enqueue_response(make_tool_call_response(
        "todo", json{{"todos", json::array({{{"id", "1"}, {"text", "buy milk"}}})}}));
    f.fake.enqueue_response(make_text_response("Todo saved."));

    bool dispatcher_called = false;
    auto agent = f.make_agent([&](const std::string&, const json&,
                                  const std::string&) {
        dispatcher_called = true;
        return R"({"ok":true})";
    });

    std::string result = agent->chat("Add a todo");
    EXPECT_FALSE(dispatcher_called);
    EXPECT_EQ(result, "Todo saved.");
}

TEST(AIAgent, MemoryIntercept) {
    AgentFixture f;
    // Model calls the "memory" tool — intercepted by agent.
    f.fake.enqueue_response(make_tool_call_response(
        "memory", json{{"action", "add"}, {"content", "remember this"}}));
    f.fake.enqueue_response(make_text_response("Remembered."));

    bool dispatcher_called = false;
    auto agent = f.make_agent([&](const std::string&, const json&,
                                  const std::string&) {
        dispatcher_called = true;
        return R"({"ok":true})";
    });

    // No memory store set — should still intercept (returns error, but not dispatcher).
    std::string result = agent->chat("Remember something");
    EXPECT_FALSE(dispatcher_called);
    EXPECT_EQ(result, "Remembered.");
}

TEST(AIAgent, CallbacksFire) {
    AgentFixture f;
    // Tool call → tool result → final text: exercises all 4 callbacks.
    f.fake.enqueue_response(
        make_tool_call_response("read_file", json{{"path", "/x"}}));
    f.fake.enqueue_response(make_text_response("Done."));

    bool on_assistant_fired = false;
    bool on_tool_call_fired = false;
    bool on_tool_result_fired = false;
    bool on_usage_fired = false;

    AgentCallbacks cbs;
    cbs.on_assistant_message = [&](const hermes::llm::Message&) {
        on_assistant_fired = true;
    };
    cbs.on_tool_call = [&](const std::string&, const json&) {
        on_tool_call_fired = true;
    };
    cbs.on_tool_result = [&](const std::string&, const std::string&) {
        on_tool_result_fired = true;
    };
    cbs.on_usage = [&](int64_t, int64_t, double) {
        on_usage_fired = true;
    };

    auto agent = f.make_agent(
        [](const std::string&, const json&, const std::string&) {
            return R"({"ok":true})";
        },
        std::move(cbs));

    agent->chat("Do it");
    EXPECT_TRUE(on_assistant_fired);
    EXPECT_TRUE(on_tool_call_fired);
    EXPECT_TRUE(on_tool_result_fired);
    EXPECT_TRUE(on_usage_fired);
}
