#include "hermes/llm/message.hpp"

#include <gtest/gtest.h>

using hermes::llm::ContentBlock;
using hermes::llm::Message;
using hermes::llm::Role;
using hermes::llm::ToolCall;
using hermes::llm::role_from_string;
using hermes::llm::role_to_string;
using json = nlohmann::json;

TEST(Message, RoleRoundTrip) {
    EXPECT_EQ("user", role_to_string(Role::User));
    EXPECT_EQ("assistant", role_to_string(Role::Assistant));
    EXPECT_EQ("system", role_to_string(Role::System));
    EXPECT_EQ("tool", role_to_string(Role::Tool));
    EXPECT_EQ(Role::User, role_from_string("user"));
    EXPECT_EQ(Role::Assistant, role_from_string("assistant"));
    EXPECT_EQ(Role::System, role_from_string("system"));
    EXPECT_EQ(Role::Tool, role_from_string("tool"));
}

TEST(Message, OpenAiSimpleTextRoundTrip) {
    Message m;
    m.role = Role::User;
    m.content_text = "hello world";
    const json j = m.to_openai();
    EXPECT_EQ(j["role"], "user");
    EXPECT_EQ(j["content"], "hello world");
    const Message back = Message::from_openai(j);
    EXPECT_EQ(back.role, Role::User);
    EXPECT_EQ(back.content_text, "hello world");
}

TEST(Message, OpenAiAssistantWithToolCall) {
    Message m;
    m.role = Role::Assistant;
    m.content_text = "let me run that";
    ToolCall tc;
    tc.id = "call_1";
    tc.name = "read_file";
    tc.arguments = json{{"path", "/tmp/x"}};
    m.tool_calls.push_back(tc);

    const json j = m.to_openai();
    EXPECT_EQ(j["role"], "assistant");
    ASSERT_TRUE(j.contains("tool_calls"));
    EXPECT_EQ(j["tool_calls"][0]["id"], "call_1");
    EXPECT_EQ(j["tool_calls"][0]["function"]["name"], "read_file");
    // arguments are stringified JSON per OpenAI spec.
    EXPECT_TRUE(j["tool_calls"][0]["function"]["arguments"].is_string());

    const Message back = Message::from_openai(j);
    EXPECT_EQ(back.role, Role::Assistant);
    ASSERT_EQ(back.tool_calls.size(), 1u);
    EXPECT_EQ(back.tool_calls[0].id, "call_1");
    EXPECT_EQ(back.tool_calls[0].name, "read_file");
    EXPECT_EQ(back.tool_calls[0].arguments["path"], "/tmp/x");
}

TEST(Message, OpenAiToolResponseRoundTrip) {
    Message m;
    m.role = Role::Tool;
    m.tool_call_id = "call_1";
    m.content_text = "result body";
    const json j = m.to_openai();
    EXPECT_EQ(j["role"], "tool");
    EXPECT_EQ(j["tool_call_id"], "call_1");
    EXPECT_EQ(j["content"], "result body");
    const Message back = Message::from_openai(j);
    EXPECT_EQ(back.role, Role::Tool);
    ASSERT_TRUE(back.tool_call_id.has_value());
    EXPECT_EQ(*back.tool_call_id, "call_1");
    EXPECT_EQ(back.content_text, "result body");
}

TEST(Message, AnthropicUserTextRoundTrip) {
    Message m;
    m.role = Role::User;
    m.content_text = "hi there";
    const json j = m.to_anthropic();
    EXPECT_EQ(j["role"], "user");
    ASSERT_TRUE(j["content"].is_array());
    EXPECT_EQ(j["content"][0]["type"], "text");
    EXPECT_EQ(j["content"][0]["text"], "hi there");
    const Message back = Message::from_anthropic(j);
    EXPECT_EQ(back.role, Role::User);
    ASSERT_FALSE(back.content_blocks.empty());
    EXPECT_EQ(back.content_blocks[0].text, "hi there");
}

TEST(Message, ContentBlockCacheControlSurvivesRoundTrip) {
    Message m;
    m.role = Role::User;
    ContentBlock b;
    b.type = "text";
    b.text = "cached prefix";
    json marker;
    marker["type"] = "ephemeral";
    b.cache_control = marker;
    m.content_blocks.push_back(b);

    const json j = m.to_anthropic();
    ASSERT_TRUE(j["content"][0].contains("cache_control"));
    EXPECT_EQ(j["content"][0]["cache_control"]["type"], "ephemeral");

    const Message back = Message::from_anthropic(j);
    ASSERT_EQ(back.content_blocks.size(), 1u);
    ASSERT_TRUE(back.content_blocks[0].cache_control.has_value());
    EXPECT_EQ((*back.content_blocks[0].cache_control)["type"], "ephemeral");
}

TEST(Message, AnthropicAssistantToolUseParsed) {
    json wire = {
        {"role", "assistant"},
        {"content",
         json::array({
             {{"type", "text"}, {"text", "calling tool"}},
             {{"type", "tool_use"},
              {"id", "tu_1"},
              {"name", "shell"},
              {"input", {{"cmd", "ls"}}}},
         })},
    };
    const Message m = Message::from_anthropic(wire);
    EXPECT_EQ(m.role, Role::Assistant);
    ASSERT_EQ(m.tool_calls.size(), 1u);
    EXPECT_EQ(m.tool_calls[0].id, "tu_1");
    EXPECT_EQ(m.tool_calls[0].name, "shell");
    EXPECT_EQ(m.tool_calls[0].arguments["cmd"], "ls");
    // The text block should still be present.
    ASSERT_EQ(m.content_blocks.size(), 1u);
    EXPECT_EQ(m.content_blocks[0].text, "calling tool");
}
