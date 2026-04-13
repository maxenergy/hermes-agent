#include "hermes/batch/hf_schema.hpp"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

namespace hermes::batch {
namespace {

TEST(HfSchemaTest, FormatsToolCallWithArguments) {
    auto s = format_tool_call("ls", nlohmann::json{{"path", "/tmp"}});
    EXPECT_NE(s.find("<tool_call>"), std::string::npos);
    EXPECT_NE(s.find("</tool_call>"), std::string::npos);
    EXPECT_NE(s.find("\"ls\""), std::string::npos);
    EXPECT_NE(s.find("\"/tmp\""), std::string::npos);
}

TEST(HfSchemaTest, FormatsToolResponseWithJsonContent) {
    auto s = format_tool_response("call_1", "ls",
                                   nlohmann::json{{"files", 3}});
    EXPECT_NE(s.find("<tool_response>"), std::string::npos);
    EXPECT_NE(s.find("call_1"), std::string::npos);
    EXPECT_NE(s.find("ls"), std::string::npos);
    EXPECT_NE(s.find("</tool_response>"), std::string::npos);
}

TEST(HfSchemaTest, BuildsHumanOnlyTrajectory) {
    std::vector<OpenAIMessage> msgs;  // empty — no back-and-forth
    auto conv = to_hf_sft_conversations(msgs, "Hi!", "SYS");
    ASSERT_EQ(conv.size(), 2u);
    EXPECT_EQ(conv[0]["from"], "system");
    EXPECT_EQ(conv[0]["value"], "SYS");
    EXPECT_EQ(conv[1]["from"], "human");
    EXPECT_EQ(conv[1]["value"], "Hi!");
}

TEST(HfSchemaTest, AssistantWithToolCallEmitsGptTurn) {
    std::vector<OpenAIMessage> msgs;

    OpenAIMessage user;
    user.role = "user";
    user.content = "List files";
    msgs.push_back(user);

    OpenAIMessage asst;
    asst.role = "assistant";
    asst.content = "let me check";
    asst.reasoning = "I will call ls";
    nlohmann::json tc;
    tc["id"] = "call_1";
    tc["function"]["name"] = "ls";
    tc["function"]["arguments"] = nlohmann::json{{"path", "/"}}.dump();
    asst.tool_calls.push_back(tc);
    msgs.push_back(asst);

    OpenAIMessage tool;
    tool.role = "tool";
    tool.tool_call_id = "call_1";
    tool.content = R"({"files":["a","b"]})";
    msgs.push_back(tool);

    auto conv = to_hf_sft_conversations(msgs, "List files", "");
    // human (seed) + gpt + tool = 3
    ASSERT_EQ(conv.size(), 3u);
    EXPECT_EQ(conv[0]["from"], "human");
    EXPECT_EQ(conv[1]["from"], "gpt");
    EXPECT_EQ(conv[2]["from"], "tool");

    auto gpt_val = conv[1]["value"].get<std::string>();
    EXPECT_NE(gpt_val.find("<think>"), std::string::npos);
    EXPECT_NE(gpt_val.find("I will call ls"), std::string::npos);
    EXPECT_NE(gpt_val.find("<tool_call>"), std::string::npos);
    EXPECT_NE(gpt_val.find("\"ls\""), std::string::npos);

    auto tool_val = conv[2]["value"].get<std::string>();
    EXPECT_NE(tool_val.find("<tool_response>"), std::string::npos);
    EXPECT_NE(tool_val.find("call_1"), std::string::npos);
}

TEST(HfSchemaTest, AssistantWithoutReasoningStillGetsThinkBlock) {
    std::vector<OpenAIMessage> msgs;
    OpenAIMessage user;
    user.role = "user";
    user.content = "hi";
    msgs.push_back(user);

    OpenAIMessage asst;
    asst.role = "assistant";
    asst.content = "hello!";
    msgs.push_back(asst);

    auto conv = to_hf_sft_conversations(msgs, "hi", "");
    ASSERT_EQ(conv.size(), 2u);
    EXPECT_EQ(conv[1]["from"], "gpt");
    auto v = conv[1]["value"].get<std::string>();
    EXPECT_NE(v.find("<think>"), std::string::npos);
    EXPECT_NE(v.find("hello!"), std::string::npos);
}

TEST(HfSchemaTest, RecordWrapsConversationsWithMetadata) {
    std::vector<OpenAIMessage> msgs;
    auto rec = to_hf_sft_record(msgs, "hi", "SYS",
                                  nlohmann::json{{"source", "unit"}});
    EXPECT_TRUE(rec.contains("conversations"));
    EXPECT_TRUE(rec["conversations"].is_array());
    EXPECT_EQ(rec["metadata"]["source"], "unit");
}

TEST(HfSchemaTest, TurnsPassThroughVerbatim) {
    std::vector<TrajectoryTurn> turns = {
        {"system", "SYS"},
        {"human", "hi"},
        {"gpt", "<think>\n</think>\nhello"},
    };
    auto conv = turns_to_conversations(turns);
    ASSERT_EQ(conv.size(), 3u);
    EXPECT_EQ(conv[2]["value"], "<think>\n</think>\nhello");
}

TEST(HfSchemaTest, ToolCallArgumentsParsedWhenString) {
    std::vector<OpenAIMessage> msgs;
    OpenAIMessage asst;
    asst.role = "assistant";
    asst.content = "";
    nlohmann::json tc;
    tc["id"] = "c1";
    tc["function"]["name"] = "echo";
    tc["function"]["arguments"] = std::string(R"({"x":42})");
    asst.tool_calls.push_back(tc);
    msgs.push_back(asst);

    auto conv = to_hf_sft_conversations(msgs, "", "");
    ASSERT_FALSE(conv.empty());
    auto gpt = conv[0]["value"].get<std::string>();
    EXPECT_NE(gpt.find("\"x\":42"), std::string::npos);
}

}  // namespace
}  // namespace hermes::batch
