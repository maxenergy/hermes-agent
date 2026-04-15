// Unit tests for hermes::environments::agent_loop — C++17 port of
// environments/agent_loop.py (portable surface).
#include "hermes/environments/agent_loop.hpp"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

using namespace hermes::environments::agent_loop;
using json = nlohmann::json;

TEST(AgentLoop, ExtractReasoningContent) {
    json msg = {{"reasoning_content", "step by step thinking"}};
    auto r = extract_reasoning_from_message(msg);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(*r, "step by step thinking");
}

TEST(AgentLoop, ExtractReasoningField) {
    json msg = {{"reasoning", "alt reasoning"}};
    auto r = extract_reasoning_from_message(msg);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(*r, "alt reasoning");
}

TEST(AgentLoop, ExtractReasoningDetailsList) {
    json msg = {{"reasoning_details", json::array({
        json{{"type", "thought"}}, json{{"text", "deep insight"}}
    })}};
    auto r = extract_reasoning_from_message(msg);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(*r, "deep insight");
}

TEST(AgentLoop, ExtractReasoningEmpty) {
    EXPECT_FALSE(extract_reasoning_from_message(json::object()).has_value());
    EXPECT_FALSE(extract_reasoning_from_message(json::array()).has_value());
    EXPECT_FALSE(extract_reasoning_from_message(json{{"reasoning_content", ""}}).has_value());
}

TEST(AgentLoop, ExtractUserTaskFirstUserMessage) {
    json msgs = json::array({
        json{{"role", "system"}, {"content", "you are X"}},
        json{{"role", "user"}, {"content", "  do something\n"}},
        json{{"role", "user"}, {"content", "ignored"}},
    });
    auto t = extract_user_task(msgs);
    ASSERT_TRUE(t.has_value());
    EXPECT_EQ(*t, "do something");
}

TEST(AgentLoop, ExtractUserTaskRespectsCap) {
    std::string big(800, 'a');
    json msgs = json::array({
        json{{"role", "user"}, {"content", big}},
    });
    auto t = extract_user_task(msgs, /*max_len=*/100);
    ASSERT_TRUE(t.has_value());
    EXPECT_EQ(t->size(), 100u);
}

TEST(AgentLoop, ExtractUserTaskMissing) {
    EXPECT_FALSE(extract_user_task(json::array()).has_value());
    EXPECT_FALSE(extract_user_task(json::array({json{{"role", "system"}, {"content", "x"}}})).has_value());
    EXPECT_FALSE(extract_user_task(json::array({json{{"role", "user"}, {"content", "   "}}})).has_value());
}

TEST(AgentLoop, ToolCallToDictObjectStyle) {
    json tc = {
        {"id", "call_42"},
        {"function", {{"name", "terminal"}, {"arguments", "{\"command\":\"ls\"}"}}},
    };
    auto d = tool_call_to_dict(tc, "fallback");
    EXPECT_EQ(d["id"].get<std::string>(), "call_42");
    EXPECT_EQ(d["type"].get<std::string>(), "function");
    EXPECT_EQ(d["function"]["name"].get<std::string>(), "terminal");
    EXPECT_EQ(d["function"]["arguments"].get<std::string>(), "{\"command\":\"ls\"}");
}

TEST(AgentLoop, ToolCallToDictDictStyleFallbackId) {
    json tc = {
        {"name", "todo"},
        {"arguments", "{}"},
    };
    auto d = tool_call_to_dict(tc, "call_x");
    EXPECT_EQ(d["id"].get<std::string>(), "call_x");
    EXPECT_EQ(d["function"]["name"].get<std::string>(), "todo");
    EXPECT_EQ(d["function"]["arguments"].get<std::string>(), "{}");
}

TEST(AgentLoop, ToolCallAccessors) {
    json tc = {
        {"id", "abc"},
        {"function", {{"name", "x"}, {"arguments", "{\"k\":1}"}}},
    };
    EXPECT_EQ(tool_call_name(tc), "x");
    EXPECT_EQ(tool_call_arguments(tc), "{\"k\":1}");
    EXPECT_EQ(tool_call_id(tc, "fb"), "abc");
    EXPECT_EQ(tool_call_id(json::object(), "fb"), "fb");
}

TEST(AgentLoop, FormatUnknownToolError) {
    std::unordered_set<std::string> valid = {"terminal", "todo", "memory"};
    auto out = format_unknown_tool_error("frobnicate", valid);
    auto parsed = json::parse(out);
    ASSERT_TRUE(parsed.contains("error"));
    EXPECT_NE(parsed["error"].get<std::string>().find("Unknown tool 'frobnicate'"), std::string::npos);
    EXPECT_NE(parsed["error"].get<std::string>().find("memory"), std::string::npos);
    EXPECT_NE(parsed["error"].get<std::string>().find("terminal"), std::string::npos);
}

TEST(AgentLoop, FormatInvalidJsonError) {
    auto out = format_invalid_json_error("Unexpected ',' at line 1");
    auto parsed = json::parse(out);
    EXPECT_NE(parsed["error"].get<std::string>().find("Invalid JSON in tool arguments"), std::string::npos);
    EXPECT_NE(parsed["error"].get<std::string>().find("Unexpected ','"), std::string::npos);
}

TEST(AgentLoop, ToolResultHasNegativeExitError) {
    EXPECT_TRUE(tool_result_has_negative_exit_error(R"({"error":"timeout","exit_code":-1})"));
    EXPECT_FALSE(tool_result_has_negative_exit_error(R"({"error":"x","exit_code":1})"));
    EXPECT_FALSE(tool_result_has_negative_exit_error(R"({"exit_code":-1})"));
    EXPECT_FALSE(tool_result_has_negative_exit_error("not json"));
}

TEST(AgentLoop, NeedsFallbackToolCallParse) {
    EXPECT_TRUE(needs_fallback_tool_call_parse(json{{"content", "<tool_call>foo</tool_call>"}}));
    EXPECT_FALSE(needs_fallback_tool_call_parse(json{{"content", "no tool call here"}}));
    EXPECT_FALSE(needs_fallback_tool_call_parse(
        json{{"content", "<tool_call>x</tool_call>"}, {"tool_calls", json::array({json::object()})}}));
}

TEST(AgentLoop, TruncateTo) {
    EXPECT_EQ(truncate_to("hello world", 5), "hello");
    EXPECT_EQ(truncate_to("hi", 5), "hi");
    EXPECT_EQ(truncate_to("", 5), "");
}

TEST(AgentLoop, AgentResultDefaults) {
    AgentResult r;
    EXPECT_TRUE(r.messages.is_array());
    EXPECT_FALSE(r.managed_state.has_value());
    EXPECT_EQ(r.turns_used, 0);
    EXPECT_FALSE(r.finished_naturally);
    EXPECT_TRUE(r.reasoning_per_turn.empty());
    EXPECT_TRUE(r.tool_errors.empty());
}

TEST(AgentLoop, ToolErrorDefaults) {
    ToolError e{};
    EXPECT_EQ(e.turn, 0);
    EXPECT_TRUE(e.tool_name.empty());
}
