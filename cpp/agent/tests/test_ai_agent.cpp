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

// ──────────────────────────────────────────────────────────────────────
// Phase-4 helper-method coverage (Python parity port)
// ──────────────────────────────────────────────────────────────────────

TEST(AIAgent, StatusCallbackFiresOnLifecycle) {
    AgentFixture f;
    f.fake.enqueue_response(make_text_response("done"));

    std::vector<std::pair<std::string, std::string>> events;
    auto agent = f.make_agent();
    agent->set_status_callback(
        [&](std::string_view phase, std::string_view msg) {
            events.emplace_back(std::string(phase), std::string(msg));
        });

    agent->chat("hi");

    // At minimum we expect a "start" and a "done" phase.
    bool saw_start = false, saw_done = false;
    for (const auto& [phase, msg] : events) {
        (void)msg;
        if (phase == "start") saw_start = true;
        if (phase == "done") saw_done = true;
    }
    EXPECT_TRUE(saw_start);
    EXPECT_TRUE(saw_done);
}

TEST(AIAgent, TelemetryCallbackFiresOnStartAndDone) {
    AgentFixture f;
    f.fake.enqueue_response(make_text_response("done"));

    std::vector<std::pair<std::string, json>> events;
    auto agent = f.make_agent();
    agent->set_telemetry_callback(
        [&](std::string_view event, const json& payload) {
            events.emplace_back(std::string(event), payload);
        });

    agent->chat("hi");

    bool saw_start = false, saw_done = false;
    for (const auto& [ev, payload] : events) {
        if (ev == "run_conversation_start") {
            saw_start = true;
            EXPECT_TRUE(payload.contains("model"));
        }
        if (ev == "run_conversation_done") {
            saw_done = true;
            EXPECT_TRUE(payload.contains("iterations"));
        }
    }
    EXPECT_TRUE(saw_start);
    EXPECT_TRUE(saw_done);
}

TEST(AIAgent, PersistUserMessageOverrideRewritesLastUser) {
    AgentFixture f;
    f.fake.enqueue_response(make_text_response("ok"));

    auto agent = f.make_agent();
    agent->set_persist_user_message_override(
        [](const std::string& original) -> std::optional<std::string> {
            return "REDACTED:" + original;
        });

    agent->chat("secret");

    // After run_conversation, the user message in the returned history
    // should be the rewritten form.  The C++ history preserves messages
    // in insertion order: [system?, user].
    const auto& msgs = agent->messages();
    bool found = false;
    for (const auto& m : msgs) {
        if (m.role == hermes::llm::Role::User) {
            EXPECT_EQ(m.content_text, std::string("REDACTED:secret"));
            found = true;
        }
    }
    EXPECT_TRUE(found);
}

TEST(AIAgent, PersistUserMessageOverrideCanOptOut) {
    AgentFixture f;
    f.fake.enqueue_response(make_text_response("ok"));

    auto agent = f.make_agent();
    agent->set_persist_user_message_override(
        [](const std::string&) -> std::optional<std::string> {
            return std::nullopt;  // keep original
        });

    agent->chat("plain");

    const auto& msgs = agent->messages();
    bool found = false;
    for (const auto& m : msgs) {
        if (m.role == hermes::llm::Role::User) {
            EXPECT_EQ(m.content_text, std::string("plain"));
            found = true;
        }
    }
    EXPECT_TRUE(found);
}

TEST(AIAgent, ActivitySummaryPopulatedAfterRun) {
    AgentFixture f;
    f.fake.enqueue_response(make_text_response("done"));
    auto agent = f.make_agent();

    // Before any run: zero-ish state.
    auto before = agent->activity_summary();
    EXPECT_EQ(before.api_call_count, 0);
    EXPECT_EQ(before.current_tool, std::string{});
    EXPECT_EQ(before.max_iterations, f.config.max_iterations);

    agent->chat("hi");

    auto after = agent->activity_summary();
    EXPECT_GT(after.last_activity_ts, 0.0);
    EXPECT_FALSE(after.last_activity_desc.empty());
    EXPECT_EQ(after.max_iterations, f.config.max_iterations);
    EXPECT_EQ(after.budget_max, f.config.max_iterations);
}

TEST(AIAgent, ActivitySummaryTracksCurrentTool) {
    AgentFixture f;
    f.fake.enqueue_response(
        make_tool_call_response("read_file", json{{"path", "/x"}}));
    f.fake.enqueue_response(make_text_response("ok"));

    // Capture current_tool observed from within the dispatcher — exactly
    // how the gateway timeout watcher reads it in production.
    std::string seen_tool_during_dispatch;
    AIAgent* agent_ptr = nullptr;
    ToolDispatcher dispatcher =
        [&](const std::string&, const json&,
            const std::string&) -> std::string {
        if (agent_ptr) {
            seen_tool_during_dispatch =
                agent_ptr->activity_summary().current_tool;
        }
        return R"({"ok":true})";
    };

    auto agent = std::make_unique<AIAgent>(
        f.config, &f.client, nullptr, nullptr, nullptr, &f.builder,
        dispatcher, std::vector<hermes::llm::ToolSchema>{}, AgentCallbacks{});
    agent->set_sleep_function([](std::chrono::milliseconds) {});
    agent_ptr = agent.get();

    agent->chat("trigger");
    EXPECT_EQ(seen_tool_during_dispatch, std::string("read_file"));

    // After the loop completes the current_tool is cleared.
    EXPECT_EQ(agent->activity_summary().current_tool, std::string{});
}

TEST(AIAgent, RateLimitStateInitiallyEmpty) {
    AgentFixture f;
    auto agent = f.make_agent();
    EXPECT_FALSE(agent->rate_limit_state().has_value());
}

TEST(AIAgent, CaptureRateLimitsStoresParsedState) {
    AgentFixture f;
    auto agent = f.make_agent();

    std::unordered_map<std::string, std::string> headers{
        {"x-ratelimit-limit-requests", "100"},
        {"x-ratelimit-remaining-requests", "50"},
        {"x-ratelimit-reset-requests", "60"},
        {"x-ratelimit-limit-tokens", "10000"},
        {"x-ratelimit-remaining-tokens", "5000"},
        {"x-ratelimit-reset-tokens", "60"},
    };
    agent->capture_rate_limits(headers, "openai");

    auto state = agent->rate_limit_state();
    ASSERT_TRUE(state.has_value());
    EXPECT_TRUE(state->has_data());
    EXPECT_EQ(state->provider, std::string("openai"));
}

TEST(AIAgent, InvalidateSystemPromptIsNoOpForBareAgent) {
    AgentFixture f;
    auto agent = f.make_agent();
    // Should not throw even when no memory store is configured.
    agent->invalidate_system_prompt();
    SUCCEED();
}

TEST(AIAgent, SetCallbacksAfterConstructionTakesEffect) {
    AgentFixture f;
    f.fake.enqueue_response(make_text_response("done"));
    auto agent = f.make_agent();

    int status_count = 0;
    int telemetry_count = 0;
    agent->set_status_callback(
        [&](std::string_view, std::string_view) { ++status_count; });
    agent->set_telemetry_callback(
        [&](std::string_view, const json&) { ++telemetry_count; });

    agent->chat("hi");

    EXPECT_GT(status_count, 0);
    EXPECT_GT(telemetry_count, 0);
}
