// Tests for Telegram reactions, forum topics, and media albums.
#include <gtest/gtest.h>

#include "../platforms/telegram.hpp"

#include <hermes/llm/llm_client.hpp>
#include <nlohmann/json.hpp>

using hermes::gateway::platforms::TelegramAdapter;
using hermes::llm::FakeHttpTransport;

TEST(TelegramReactions, SetReactionPostsCorrectPayload) {
    FakeHttpTransport fake;
    fake.enqueue_response({200, R"({"ok":true})", {}});
    TelegramAdapter::Config cfg;
    cfg.bot_token = "TKN";
    TelegramAdapter adapter(cfg, &fake);

    EXPECT_TRUE(adapter.set_reaction("123", 42, "👍"));
    ASSERT_EQ(fake.requests().size(), 1u);
    EXPECT_NE(fake.requests()[0].url.find("/setMessageReaction"),
              std::string::npos);
    auto body = nlohmann::json::parse(fake.requests()[0].body);
    EXPECT_EQ(body["chat_id"], "123");
    EXPECT_EQ(body["message_id"], 42);
    ASSERT_TRUE(body["reaction"].is_array());
    EXPECT_EQ(body["reaction"][0]["type"], "emoji");
    EXPECT_EQ(body["reaction"][0]["emoji"], "👍");
}

TEST(TelegramReactions, ParseForumTopicId) {
    nlohmann::json msg = {{"message_thread_id", 7}};
    auto id = TelegramAdapter::parse_forum_topic(msg);
    ASSERT_TRUE(id.has_value());
    EXPECT_EQ(*id, 7);

    auto none = TelegramAdapter::parse_forum_topic(nlohmann::json::object());
    EXPECT_FALSE(none.has_value());
}

TEST(TelegramReactions, ParseMediaGroupId) {
    nlohmann::json msg = {{"media_group_id", "abc"}};
    auto g = TelegramAdapter::parse_media_group_id(msg);
    ASSERT_TRUE(g.has_value());
    EXPECT_EQ(*g, "abc");

    EXPECT_FALSE(
        TelegramAdapter::parse_media_group_id(nlohmann::json::object())
            .has_value());
}
