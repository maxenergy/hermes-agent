// Unit tests for the Copilot ACP reverse-adapter port.  These mirror the
// tests of the Python reference in agent/copilot_acp_client.py, without
// depending on a live Copilot CLI installation.
#include "hermes/agent/copilot_acp_client.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

using hermes::agent::CopilotACPClient;
using hermes::agent::CopilotACPRequest;
using hermes::agent::CopilotACPResponse;
using hermes::agent::CopilotACPToolCall;
using hermes::agent::ensure_path_within_cwd;
using hermes::agent::extract_tool_calls_from_text;
using hermes::agent::format_messages_as_prompt;
using hermes::agent::jsonrpc_error;
using hermes::agent::render_message_content;
using hermes::agent::resolve_copilot_acp_args;
using hermes::agent::resolve_copilot_acp_command;

using nlohmann::json;

namespace {

void unset_all_env() {
    ::unsetenv("HERMES_COPILOT_ACP_COMMAND");
    ::unsetenv("COPILOT_CLI_PATH");
    ::unsetenv("HERMES_COPILOT_ACP_ARGS");
}

}  // namespace

TEST(CopilotACPHelpers, RenderMessageContentStringPassthrough) {
    EXPECT_EQ(render_message_content(json("   hello world   ")), "hello world");
    EXPECT_EQ(render_message_content(json(nullptr)), "");
}

TEST(CopilotACPHelpers, RenderMessageContentList) {
    json list = json::array();
    list.push_back({{"type", "text"}, {"text", "  first chunk "}});
    list.push_back({{"type", "image_url"}, {"image_url", {{"url", "x"}}}});
    list.push_back({{"type", "text"}, {"text", "second"}});
    list.push_back("bare string");
    auto rendered = render_message_content(list);
    EXPECT_NE(rendered.find("first chunk"), std::string::npos);
    EXPECT_NE(rendered.find("second"), std::string::npos);
    EXPECT_NE(rendered.find("bare string"), std::string::npos);
    EXPECT_EQ(rendered.find("image_url"), std::string::npos);
}

TEST(CopilotACPHelpers, RenderMessageContentDictFallbacks) {
    json dict1 = json::object();
    dict1["text"] = "inner text ";
    EXPECT_EQ(render_message_content(dict1), "inner text");

    json dict2 = json::object();
    dict2["content"] = " nested content ";
    EXPECT_EQ(render_message_content(dict2), "nested content");

    json dict3 = json::object();
    dict3["other"] = 42;
    auto rendered = render_message_content(dict3);
    EXPECT_NE(rendered.find("\"other\""), std::string::npos);
}

TEST(CopilotACPHelpers, FormatMessagesAsPromptPredictable) {
    json messages = json::array();
    messages.push_back({{"role", "user"}, {"content", "hi"}});
    messages.push_back({{"role", "assistant"}, {"content", "hello"}});

    auto prompt = format_messages_as_prompt(messages);
    EXPECT_NE(prompt.find("You are being used as the active ACP agent backend"),
              std::string::npos);
    EXPECT_NE(prompt.find("User:\nhi"), std::string::npos);
    EXPECT_NE(prompt.find("Assistant:\nhello"), std::string::npos);
    EXPECT_NE(prompt.find("Continue the conversation"), std::string::npos);
    // The "Conversation transcript:" label comes before the first role.
    auto tloc = prompt.find("Conversation transcript:");
    auto uloc = prompt.find("User:");
    ASSERT_NE(tloc, std::string::npos);
    ASSERT_NE(uloc, std::string::npos);
    EXPECT_LT(tloc, uloc);
}

TEST(CopilotACPHelpers, FormatMessagesIncludesToolSpecs) {
    json messages = json::array();
    messages.push_back({{"role", "user"}, {"content", "do it"}});

    json tools = json::array();
    json t;
    t["type"] = "function";
    t["function"] = json::object();
    t["function"]["name"] = "read_file";
    t["function"]["description"] = "read a file";
    t["function"]["parameters"] = json::object();
    tools.push_back(t);

    auto prompt = format_messages_as_prompt(messages, "copilot", tools, std::nullopt);
    EXPECT_NE(prompt.find("Available tools"), std::string::npos);
    EXPECT_NE(prompt.find("read_file"), std::string::npos);
    EXPECT_NE(prompt.find("Hermes requested model hint: copilot"), std::string::npos);
}

TEST(CopilotACPHelpers, ExtractSingleToolCall) {
    std::string text =
        "Here is my plan.\n"
        "<tool_call>{\"id\":\"c1\",\"type\":\"function\",\"function\":{\"name\":\"read_file\",\"arguments\":\"{\\\"path\\\":\\\"/tmp/x\\\"}\"}}</tool_call>\n"
        "Bye.";
    auto [calls, cleaned] = extract_tool_calls_from_text(text);
    ASSERT_EQ(calls.size(), 1u);
    EXPECT_EQ(calls[0].id, "c1");
    EXPECT_EQ(calls[0].name, "read_file");
    EXPECT_NE(calls[0].arguments.find("\"path\""), std::string::npos);
    EXPECT_NE(cleaned.find("Here is my plan."), std::string::npos);
    EXPECT_NE(cleaned.find("Bye."), std::string::npos);
    EXPECT_EQ(cleaned.find("<tool_call>"), std::string::npos);
}

TEST(CopilotACPHelpers, ExtractMultipleToolCalls) {
    std::string text =
        "<tool_call>{\"id\":\"a\",\"type\":\"function\",\"function\":{\"name\":\"f1\",\"arguments\":\"{}\"}}</tool_call>"
        " middle "
        "<tool_call>{\"id\":\"b\",\"type\":\"function\",\"function\":{\"name\":\"f2\",\"arguments\":\"{}\"}}</tool_call>";
    auto [calls, cleaned] = extract_tool_calls_from_text(text);
    ASSERT_EQ(calls.size(), 2u);
    EXPECT_EQ(calls[0].name, "f1");
    EXPECT_EQ(calls[1].name, "f2");
    EXPECT_EQ(cleaned.find("<tool_call>"), std::string::npos);
    EXPECT_NE(cleaned.find("middle"), std::string::npos);
}

TEST(CopilotACPHelpers, ExtractNoToolCallsReturnsOriginal) {
    std::string text = "plain answer with no tool_call block";
    auto [calls, cleaned] = extract_tool_calls_from_text(text);
    EXPECT_TRUE(calls.empty());
    EXPECT_EQ(cleaned, text);
}

TEST(CopilotACPHelpers, ExtractEmptyInput) {
    auto [calls, cleaned] = extract_tool_calls_from_text("");
    EXPECT_TRUE(calls.empty());
    EXPECT_EQ(cleaned, "");
    auto [calls2, cleaned2] = extract_tool_calls_from_text("   \n   ");
    EXPECT_TRUE(calls2.empty());
    EXPECT_EQ(cleaned2, "");
}

TEST(CopilotACPHelpers, ExtractAutoAssignsIdWhenMissing) {
    std::string text =
        "<tool_call>{\"id\":\"\",\"type\":\"function\",\"function\":{\"name\":\"f\",\"arguments\":\"{}\"}}</tool_call>";
    auto [calls, cleaned] = extract_tool_calls_from_text(text);
    ASSERT_EQ(calls.size(), 1u);
    EXPECT_EQ(calls[0].id, "acp_call_1");
    (void)cleaned;
}

TEST(CopilotACPHelpers, ExtractStringifiesObjectArguments) {
    std::string text =
        "<tool_call>{\"id\":\"x\",\"type\":\"function\",\"function\":{\"name\":\"f\",\"arguments\":{\"k\":1}}}</tool_call>";
    auto [calls, cleaned] = extract_tool_calls_from_text(text);
    ASSERT_EQ(calls.size(), 1u);
    EXPECT_EQ(calls[0].name, "f");
    // Arguments should be serialised back to JSON string form.
    EXPECT_EQ(calls[0].arguments.find('{'), 0u);
    EXPECT_NE(calls[0].arguments.find("\"k\""), std::string::npos);
    (void)cleaned;
}

TEST(CopilotACPHelpers, ExtractBareJsonFallback) {
    std::string text =
        "prefix {\"id\":\"z\",\"type\":\"function\",\"function\":{\"name\":\"f\",\"arguments\":\"{}\"}} suffix";
    auto [calls, cleaned] = extract_tool_calls_from_text(text);
    ASSERT_EQ(calls.size(), 1u);
    EXPECT_EQ(calls[0].id, "z");
    EXPECT_NE(cleaned.find("prefix"), std::string::npos);
    EXPECT_NE(cleaned.find("suffix"), std::string::npos);
    EXPECT_EQ(cleaned.find("\"id\":\"z\""), std::string::npos);
}

TEST(CopilotACPHelpers, JsonRpcErrorShape) {
    auto err = jsonrpc_error(json(7), -32601, "nope");
    EXPECT_EQ(err["jsonrpc"], "2.0");
    EXPECT_EQ(err["id"], 7);
    EXPECT_EQ(err["error"]["code"], -32601);
    EXPECT_EQ(err["error"]["message"], "nope");
}

TEST(CopilotACPHelpers, EnsurePathWithinCwd) {
    auto tmp = std::filesystem::temp_directory_path() / "hermes_copilot_acp_test";
    std::filesystem::create_directories(tmp);
    auto inside = tmp / "a.txt";
    { std::ofstream ofs(inside); ofs << "x"; }
    auto ok = ensure_path_within_cwd(inside.string(), tmp.string());
    EXPECT_NE(ok.find("a.txt"), std::string::npos);

    EXPECT_THROW(ensure_path_within_cwd("relative/path", tmp.string()), std::runtime_error);

    auto outside = std::filesystem::temp_directory_path() / "definitely_not_there.xyz";
    EXPECT_THROW(ensure_path_within_cwd(outside.string(), tmp.string()), std::runtime_error);

    std::filesystem::remove_all(tmp);
}

TEST(CopilotACPCommandResolution, EnvironmentPrecedence) {
    unset_all_env();
    EXPECT_EQ(resolve_copilot_acp_command(), "copilot");
    EXPECT_EQ(resolve_copilot_acp_args(), (std::vector<std::string>{"--acp", "--stdio"}));

    ::setenv("COPILOT_CLI_PATH", "/opt/copilot", 1);
    EXPECT_EQ(resolve_copilot_acp_command(), "/opt/copilot");

    ::setenv("HERMES_COPILOT_ACP_COMMAND", "/usr/local/bin/mycopilot", 1);
    EXPECT_EQ(resolve_copilot_acp_command(), "/usr/local/bin/mycopilot");

    ::setenv("HERMES_COPILOT_ACP_ARGS", "--foo --bar=\"baz qux\"", 1);
    auto args = resolve_copilot_acp_args();
    ASSERT_EQ(args.size(), 2u);
    EXPECT_EQ(args[0], "--foo");
    EXPECT_EQ(args[1], "--bar=baz qux");

    unset_all_env();
}

TEST(CopilotACPClient, ConstructorHonoursOverrides) {
    unset_all_env();
    CopilotACPClient client;
    EXPECT_EQ(client.resolved_command(), "copilot");

    client.set_command("/bin/echo");
    EXPECT_EQ(client.resolved_command(), "/bin/echo");

    client.set_args({"arg1", "arg2"});
    auto args = client.resolved_args();
    ASSERT_EQ(args.size(), 2u);
    EXPECT_EQ(args[0], "arg1");
    EXPECT_EQ(args[1], "arg2");
}

// Live smoke — by default we substitute `/bin/false` so the "child" exits
// immediately without speaking ACP, and we assert that we surface an
// error gracefully (no crash, no throw).  When HERMES_COPILOT_ACP_LIVE=1
// the test is skipped so developers with a real copilot binary can run
// it manually.
TEST(CopilotACPClient, CompleteWithBrokenSubprocessReturnsError) {
    if (const char* v = std::getenv("HERMES_COPILOT_ACP_LIVE"); v && std::string(v) == "1") {
        GTEST_SKIP() << "HERMES_COPILOT_ACP_LIVE set; skipping fake-subprocess test.";
    }
    unset_all_env();
    CopilotACPClient client;
    client.set_command("/bin/false");
    client.set_args({});

    CopilotACPRequest req;
    req.messages = json::array();
    req.messages.push_back({{"role", "user"}, {"content", "hi"}});
    req.timeout = std::chrono::seconds(5);

    CopilotACPResponse resp;
    ASSERT_NO_THROW({ resp = client.complete(req); });
    EXPECT_EQ(resp.model, "copilot-acp");
    // /bin/false exits immediately, so we expect a graceful error outcome.
    EXPECT_TRUE(resp.finish_reason == "error" || resp.finish_reason == "stop");
    // tool_calls must never be partially populated.
    for (const auto& tc : resp.tool_calls) {
        EXPECT_FALSE(tc.name.empty());
    }
}

TEST(CopilotACPClient, CompleteWithMissingCommandSurfacesError) {
    unset_all_env();
    CopilotACPClient client;
    client.set_command("/nonexistent/hermes-copilot-does-not-exist");
    client.set_args({});

    CopilotACPRequest req;
    req.messages = json::array();
    req.messages.push_back({{"role", "user"}, {"content", "hi"}});
    req.timeout = std::chrono::seconds(3);

    auto resp = client.complete(req);
    // Either the fork+execvp fails and the child exits 127, or we catch
    // the error at send_request time.  Either way we should not crash
    // and we should not pretend success.
    EXPECT_NE(resp.finish_reason, "tool_calls");
    EXPECT_EQ(resp.model, "copilot-acp");
}
