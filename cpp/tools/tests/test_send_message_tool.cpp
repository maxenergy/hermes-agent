#include "hermes/tools/registry.hpp"
#include "hermes/tools/send_message_tool.hpp"

#include <gtest/gtest.h>

#include <stdexcept>

using namespace hermes::tools;

namespace {

class SendMessageToolTest : public ::testing::Test {
protected:
    void SetUp() override {
        ToolRegistry::instance().clear();
        register_send_message_tools();
    }
    void TearDown() override { ToolRegistry::instance().clear(); }
};

TEST(ParseTargetTest, HappyPath) {
    auto pt = parse_target("telegram:12345:67890");
    EXPECT_EQ(pt.platform, "telegram");
    EXPECT_EQ(pt.chat_id, "12345");
    EXPECT_EQ(pt.thread_id, "67890");
}

TEST(ParseTargetTest, EmptyThreadIdAllowed) {
    auto pt = parse_target("discord:chan123:");
    EXPECT_EQ(pt.platform, "discord");
    EXPECT_EQ(pt.chat_id, "chan123");
    EXPECT_EQ(pt.thread_id, "");
}

TEST(ParseTargetTest, InvalidFormatThrows) {
    EXPECT_THROW(parse_target("nocolons"), std::invalid_argument);
    EXPECT_THROW(parse_target("one:colon"), std::invalid_argument);
    EXPECT_THROW(parse_target(":empty_platform:tid"), std::invalid_argument);
}

TEST_F(SendMessageToolTest, SendActionReturnsGatewayNotRunning) {
    // Without a gateway runner set, send returns a clear error.
    auto result = ToolRegistry::instance().dispatch(
        "send_message",
        {{"action", "send"},
         {"target", "telegram:123:456"},
         {"content", "hello"}},
        {});
    auto parsed = nlohmann::json::parse(result);
    EXPECT_TRUE(parsed.contains("error"));
    EXPECT_NE(parsed["error"].get<std::string>().find("gateway not running"),
              std::string::npos);
}

TEST_F(SendMessageToolTest, ListActionReturnsGatewayNotRunning) {
    auto result = ToolRegistry::instance().dispatch(
        "send_message", {{"action", "list"}}, {});
    auto parsed = nlohmann::json::parse(result);
    EXPECT_TRUE(parsed.contains("error"));
    EXPECT_NE(parsed["error"].get<std::string>().find("gateway not running"),
              std::string::npos);
}

TEST_F(SendMessageToolTest, InvalidTargetFormatReturnsError) {
    auto result = ToolRegistry::instance().dispatch(
        "send_message",
        {{"action", "send"},
         {"target", "bad-format"},
         {"content", "hi"}},
        {});
    auto parsed = nlohmann::json::parse(result);
    EXPECT_TRUE(parsed.contains("error"));
}

}  // namespace
