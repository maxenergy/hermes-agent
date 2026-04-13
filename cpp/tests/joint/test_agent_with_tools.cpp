// Joint integration tests — AIAgent + ToolRegistry + tool handlers.
//
// The entire stack is driven by FakeHttpTransport: we enqueue an OpenAI-style
// tool-call response, then a follow-up text response, and assert that the
// ToolDispatcher actually fired and that the final assistant text reaches the
// caller.

#include "hermes/agent/ai_agent.hpp"
#include "hermes/llm/llm_client.hpp"
#include "hermes/llm/openai_client.hpp"
#include "hermes/tools/registry.hpp"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <memory>
#include <string>

using hermes::agent::AgentConfig;
using hermes::agent::AIAgent;
using hermes::agent::PromptBuilder;
using hermes::agent::ToolDispatcher;
using hermes::llm::FakeHttpTransport;
using hermes::llm::HttpTransport;
using hermes::llm::OpenAIClient;
using hermes::tools::ToolContext;
using hermes::tools::ToolEntry;
using hermes::tools::ToolHandler;
using hermes::tools::ToolRegistry;
using json = nlohmann::json;

namespace {

HttpTransport::Response make_text_response(const std::string& text) {
    HttpTransport::Response r;
    r.status_code = 200;
    r.body = json{
        {"id", "cmpl"},
        {"choices", json::array({{
            {"index", 0},
            {"finish_reason", "stop"},
            {"message", {{"role", "assistant"}, {"content", text}}},
        }})},
        {"usage", {{"prompt_tokens", 3}, {"completion_tokens", 1}}},
    }.dump();
    return r;
}

HttpTransport::Response make_tool_call_response(const std::string& name,
                                                 const json& args,
                                                 const std::string& id = "call_1") {
    HttpTransport::Response r;
    r.status_code = 200;
    r.body = json{
        {"id", "cmpl"},
        {"choices", json::array({{
            {"index", 0},
            {"finish_reason", "tool_calls"},
            {"message", {
                {"role", "assistant"},
                {"content", nullptr},
                {"tool_calls", json::array({{
                    {"id", id},
                    {"type", "function"},
                    {"function", {{"name", name}, {"arguments", args.dump()}}},
                }})},
            }},
        }})},
        {"usage", {{"prompt_tokens", 3}, {"completion_tokens", 1}}},
    }.dump();
    return r;
}

ToolEntry make_tool(const std::string& name, ToolHandler h,
                    const std::string& toolset = "joint") {
    ToolEntry e;
    e.name = name;
    e.toolset = toolset;
    e.description = "joint test tool";
    e.schema = json::object();
    e.schema["name"] = name;
    e.schema["parameters"] = {{"type", "object"}, {"properties", json::object()}};
    e.handler = std::move(h);
    return e;
}

struct Fixture {
    FakeHttpTransport fake;
    OpenAIClient client{&fake, "sk-test"};
    PromptBuilder builder;
    AgentConfig config;

    Fixture() {
        ToolRegistry::instance().clear();
        config.model = "gpt-4o";
        config.max_retries = 0;
        config.max_iterations = 20;
    }
    ~Fixture() { ToolRegistry::instance().clear(); }

    std::unique_ptr<AIAgent> make(ToolDispatcher d = nullptr) {
        auto a = std::make_unique<AIAgent>(
            config, &client, nullptr, nullptr, nullptr, &builder,
            std::move(d));
        a->set_sleep_function([](std::chrono::milliseconds) {});
        return a;
    }
};

ToolDispatcher registry_dispatcher() {
    return [](const std::string& name, const json& args,
              const std::string& task_id) {
        ToolContext ctx;
        ctx.task_id = task_id;
        return ToolRegistry::instance().dispatch(name, args, ctx);
    };
}

}  // namespace

TEST(JointAgentTools, SingleReadFileCall) {
    Fixture f;
    ToolRegistry::instance().register_tool(make_tool(
        "read_file", [](const json& args, const ToolContext&) {
            return json{{"contents", "hi " + args.value("path", "")}}.dump();
        }));

    f.fake.enqueue_response(
        make_tool_call_response("read_file", json{{"path", "/tmp/x"}}));
    f.fake.enqueue_response(make_text_response("Got it."));

    auto agent = f.make(registry_dispatcher());
    auto out = agent->chat("read /tmp/x");
    EXPECT_EQ(out, "Got it.");
    EXPECT_EQ(f.fake.requests().size(), 2u);
}

TEST(JointAgentTools, ThreeStepToolChain) {
    Fixture f;
    int calls = 0;
    for (const auto& n : {"step_a", "step_b", "step_c"}) {
        ToolRegistry::instance().register_tool(make_tool(
            n, [&](const json&, const ToolContext&) {
                calls++;
                return json{{"ok", true}}.dump();
            }));
    }

    f.fake.enqueue_response(make_tool_call_response("step_a", json::object(), "c1"));
    f.fake.enqueue_response(make_tool_call_response("step_b", json::object(), "c2"));
    f.fake.enqueue_response(make_tool_call_response("step_c", json::object(), "c3"));
    f.fake.enqueue_response(make_text_response("All done."));

    auto agent = f.make(registry_dispatcher());
    auto out = agent->chat("do three");
    EXPECT_EQ(out, "All done.");
    EXPECT_EQ(calls, 3);
}

TEST(JointAgentTools, IterationCapEnforced) {
    Fixture f;
    f.config.max_iterations = 2;
    ToolRegistry::instance().register_tool(make_tool(
        "spin", [](const json&, const ToolContext&) {
            return std::string(R"({"ok":true})");
        }));

    for (int i = 0; i < 8; ++i) {
        f.fake.enqueue_response(
            make_tool_call_response("spin", json::object(),
                                    "c" + std::to_string(i)));
    }

    auto agent = f.make(registry_dispatcher());
    auto result = agent->run_conversation("loop");
    EXPECT_FALSE(result.completed);
}

TEST(JointAgentTools, ToolErrorPropagates) {
    Fixture f;
    ToolRegistry::instance().register_tool(make_tool(
        "bad", [](const json&, const ToolContext&) -> std::string {
            throw std::runtime_error("disk on fire");
        }));

    f.fake.enqueue_response(make_tool_call_response("bad", json::object()));
    f.fake.enqueue_response(make_text_response("Recovered."));

    auto agent = f.make(registry_dispatcher());
    auto out = agent->chat("go");
    EXPECT_EQ(out, "Recovered.");
    const auto& msgs = agent->messages();
    bool found_err = false;
    for (const auto& m : msgs) {
        if (m.content_text.find("error") != std::string::npos ||
            m.content_text.find("disk on fire") != std::string::npos) {
            found_err = true;
            break;
        }
    }
    EXPECT_TRUE(found_err);
}

TEST(JointAgentTools, TodoInterceptedByAgent) {
    Fixture f;
    bool registry_hit = false;
    ToolRegistry::instance().register_tool(make_tool(
        "todo", [&](const json&, const ToolContext&) {
            registry_hit = true;
            return std::string(R"({"ok":true})");
        }));

    f.fake.enqueue_response(make_tool_call_response(
        "todo", json{{"todos", json::array({{{"id", "1"}, {"text", "buy"}}})}}));
    f.fake.enqueue_response(make_text_response("ok"));

    auto agent = f.make(registry_dispatcher());
    agent->chat("add todo");
    EXPECT_FALSE(registry_hit);
}

TEST(JointAgentTools, MemoryInterceptedByAgent) {
    Fixture f;
    bool registry_hit = false;
    ToolRegistry::instance().register_tool(make_tool(
        "memory", [&](const json&, const ToolContext&) {
            registry_hit = true;
            return std::string(R"({"ok":true})");
        }));

    f.fake.enqueue_response(make_tool_call_response(
        "memory", json{{"action", "add"}, {"content", "x"}}));
    f.fake.enqueue_response(make_text_response("done"));

    auto agent = f.make(registry_dispatcher());
    agent->chat("remember");
    EXPECT_FALSE(registry_hit);
}
