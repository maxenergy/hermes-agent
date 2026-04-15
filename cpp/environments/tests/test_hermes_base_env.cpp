// Unit tests for hermes::environments::base_env — portable surface of
// environments/hermes_base_env.py.
#include "hermes/environments/hermes_base_env.hpp"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <cstdlib>

using namespace hermes::environments::base_env;
using json = nlohmann::json;

TEST(HermesBaseEnv, DefaultConfigPassesValidation) {
    HermesAgentEnvConfig c;
    EXPECT_TRUE(c.validate().empty());
    EXPECT_EQ(c.max_agent_turns, 30);
    EXPECT_EQ(c.terminal_backend, "local");
    EXPECT_EQ(c.terminal_timeout, 120);
    EXPECT_EQ(c.terminal_lifetime, 3600);
    EXPECT_EQ(c.tool_pool_size, 128);
    EXPECT_EQ(c.tool_call_parser, "hermes");
    EXPECT_DOUBLE_EQ(c.agent_temperature, 1.0);
}

TEST(HermesBaseEnv, ValidateCatchesBadValues) {
    HermesAgentEnvConfig c;
    c.max_agent_turns = 0;
    c.agent_temperature = -0.1;
    c.terminal_timeout = 0;
    c.terminal_lifetime = -1;
    c.tool_pool_size = 0;
    c.terminal_backend = "wat";
    c.enabled_toolsets = std::vector<std::string>{"a"};
    c.distribution = std::string{"foo"};
    auto errs = c.validate();
    EXPECT_GE(errs.size(), 7u);
    bool saw_distribution = false;
    for (auto& e : errs) {
        if (e.field == "distribution") saw_distribution = true;
    }
    EXPECT_TRUE(saw_distribution);
}

TEST(HermesBaseEnv, BuildBudgetConfigUsesFields) {
    HermesAgentEnvConfig c;
    c.default_result_size_chars = 10;
    c.turn_budget_chars = 20;
    c.preview_size_chars = 30;
    std::unordered_map<std::string, int> overrides = {{"terminal", 999}};
    c.tool_result_overrides = overrides;
    auto bc = c.build_budget_config();
    EXPECT_EQ(bc.default_result_size, 10);
    EXPECT_EQ(bc.turn_budget, 20);
    EXPECT_EQ(bc.preview_size, 30);
    ASSERT_EQ(bc.tool_overrides.count("terminal"), 1u);
    EXPECT_EQ(bc.tool_overrides["terminal"], 999);
}

TEST(HermesBaseEnv, BudgetConfigToJson) {
    BudgetConfig bc;
    bc.default_result_size = 1;
    bc.turn_budget = 2;
    bc.preview_size = 3;
    bc.tool_overrides = {{"x", 4}};
    auto j = bc.to_json();
    EXPECT_EQ(j["default_result_size"].get<int>(), 1);
    EXPECT_EQ(j["turn_budget"].get<int>(), 2);
    EXPECT_EQ(j["preview_size"].get<int>(), 3);
    EXPECT_EQ(j["tool_overrides"]["x"].get<int>(), 4);
}

TEST(HermesBaseEnv, JsonRoundTrip) {
    json src;
    src["enabled_toolsets"] = json::array({"file", "web"});
    src["distribution"] = nullptr;
    src["max_agent_turns"] = 10;
    src["agent_temperature"] = 0.7;
    src["terminal_backend"] = "docker";
    src["terminal_timeout"] = 60;
    src["terminal_lifetime"] = 1200;
    src["tool_pool_size"] = 16;
    src["tool_call_parser"] = "qwen";
    src["default_result_size_chars"] = 1000;
    src["turn_budget_chars"] = 5000;
    src["preview_size_chars"] = 500;
    src["tool_result_overrides"] = {{"terminal", 7000}};
    src["extra_body"] = {{"provider", {{"order", json::array({"Together"})}}}};

    auto cfg = HermesAgentEnvConfig::from_json(src);
    EXPECT_TRUE(cfg.enabled_toolsets.has_value());
    EXPECT_EQ(cfg.enabled_toolsets->size(), 2u);
    EXPECT_EQ(cfg.max_agent_turns, 10);
    EXPECT_EQ(cfg.terminal_backend, "docker");
    EXPECT_EQ(cfg.tool_call_parser, "qwen");
    ASSERT_TRUE(cfg.tool_result_overrides.has_value());
    EXPECT_EQ((*cfg.tool_result_overrides)["terminal"], 7000);
    ASSERT_TRUE(cfg.extra_body.has_value());
    EXPECT_TRUE(cfg.extra_body->is_object());

    // Round-trip preserves the round-trippable shape.
    auto out = cfg.to_json();
    EXPECT_EQ(out["max_agent_turns"].get<int>(), 10);
    EXPECT_EQ(out["terminal_backend"].get<std::string>(), "docker");
    EXPECT_EQ(out["tool_call_parser"].get<std::string>(), "qwen");
    EXPECT_TRUE(out.contains("extra_body"));
}

TEST(HermesBaseEnv, UseManagedServer) {
    EXPECT_TRUE(use_managed_server("vllm"));
    EXPECT_TRUE(use_managed_server("sglang"));
    EXPECT_TRUE(use_managed_server("managed"));
    EXPECT_FALSE(use_managed_server("openai"));
    EXPECT_FALSE(use_managed_server(""));
    EXPECT_FALSE(use_managed_server("anthropic"));
}

TEST(HermesBaseEnv, ApplyTerminalEnvSetsVars) {
    HermesAgentEnvConfig c;
    c.terminal_backend = "modal";
    c.terminal_timeout = 222;
    c.terminal_lifetime = 9999;
    auto pairs = c.apply_terminal_env();
    ASSERT_EQ(pairs.size(), 3u);

    EXPECT_STREQ(::getenv("TERMINAL_ENV"), "modal");
    EXPECT_STREQ(::getenv("TERMINAL_TIMEOUT"), "222");
    EXPECT_STREQ(::getenv("TERMINAL_LIFETIME_SECONDS"), "9999");

    // Cleanup so other tests aren't affected.
    ::unsetenv("TERMINAL_ENV");
    ::unsetenv("TERMINAL_TIMEOUT");
    ::unsetenv("TERMINAL_LIFETIME_SECONDS");
}

TEST(HermesBaseEnv, FormatTrajectoryForDisplaySimple) {
    json msgs = json::array({
        json{{"role", "system"}, {"content", "you are a helpful agent"}},
        json{{"role", "user"}, {"content", "hello"}},
        json{{"role", "assistant"},
             {"content", "hi"},
             {"reasoning_content", "the user said hi"}},
    });
    auto out = format_trajectory_for_display(msgs);
    EXPECT_NE(out.find("[SYSTEM]"), std::string::npos);
    EXPECT_NE(out.find("[USER]"), std::string::npos);
    EXPECT_NE(out.find("[ASSISTANT thinking]"), std::string::npos);
    EXPECT_NE(out.find("[ASSISTANT]"), std::string::npos);
    EXPECT_NE(out.find("the user said hi"), std::string::npos);
}

TEST(HermesBaseEnv, FormatTrajectoryToolCallsAndResults) {
    json msgs = json::array({
        json{{"role", "assistant"},
             {"content", ""},
             {"tool_calls", json::array({
                 json{{"id", "1"}, {"function",
                       json{{"name", "terminal"}, {"arguments", "{\"command\":\"ls\"}"}}}},
             })}},
        json{{"role", "tool"}, {"tool_call_id", "1"},
             {"content", std::string(800, 'x')}},
    });
    auto out = format_trajectory_for_display(msgs);
    EXPECT_NE(out.find("[TOOL CALL] terminal({\"command\":\"ls\"})"), std::string::npos);
    EXPECT_NE(out.find("[TOOL RESULT]"), std::string::npos);
    EXPECT_NE(out.find("..."), std::string::npos);
}

TEST(HermesBaseEnv, FormatTrajectoryReasoningTruncatedTo300) {
    std::string big(500, 'r');
    json msgs = json::array({
        json{{"role", "assistant"}, {"reasoning_content", big}, {"content", ""}},
    });
    auto out = format_trajectory_for_display(msgs);
    // Should contain truncation marker at length 300.
    EXPECT_NE(out.find(std::string(300, 'r') + "..."), std::string::npos);
}

TEST(HermesBaseEnv, FormatToolErrorSummary) {
    json err = {
        {"turn", 4}, {"tool", "terminal"},
        {"args", "really long arguments string that is more than eighty characters in size for sure now"},
        {"error", "BadCommandError: file not found"},
    };
    auto s = format_tool_error_summary(err);
    EXPECT_NE(s.find("[turn 4]"), std::string::npos);
    EXPECT_NE(s.find("terminal("), std::string::npos);
    EXPECT_NE(s.find("BadCommandError"), std::string::npos);
    // args truncated to 80 chars (no ellipsis)
    EXPECT_EQ(s.find("for sure now"), std::string::npos);
}

TEST(HermesBaseEnv, FormatToolErrorDetailsJoinsLines) {
    json errs = json::array({
        json{{"turn", 1}, {"tool", "x"}, {"args", "a"}, {"error", "e1"}},
        json{{"turn", 2}, {"tool", "y"}, {"args", "b"}, {"error", "e2"}},
    });
    auto s = format_tool_error_details(errs);
    auto nl = s.find('\n');
    ASSERT_NE(nl, std::string::npos);
    EXPECT_NE(s.find("[turn 1]"), std::string::npos);
    EXPECT_NE(s.find("[turn 2]"), std::string::npos);
}

TEST(HermesBaseEnv, FormatTrajectoryEmpty) {
    EXPECT_EQ(format_trajectory_for_display(json::array()), "");
    EXPECT_EQ(format_trajectory_for_display(json::object()), "");
}
