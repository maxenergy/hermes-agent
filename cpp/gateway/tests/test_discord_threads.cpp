// Tests for Discord thread creation and reactions.
#include <gtest/gtest.h>

#include "../platforms/discord.hpp"

#include <hermes/llm/llm_client.hpp>
#include <nlohmann/json.hpp>

using hermes::gateway::platforms::DiscordAdapter;
using hermes::llm::FakeHttpTransport;

TEST(DiscordThreads, CreateThreadPostsToChannelThreads) {
    FakeHttpTransport fake;
    fake.enqueue_response({201, R"({"id":"9"})", {}});
    DiscordAdapter::Config cfg;
    cfg.bot_token = "TKN";
    DiscordAdapter adapter(cfg, &fake);

    EXPECT_TRUE(adapter.create_thread("111", "design discussion"));
    ASSERT_EQ(fake.requests().size(), 1u);
    EXPECT_NE(fake.requests()[0].url.find("/channels/111/threads"),
              std::string::npos);
    auto body = nlohmann::json::parse(fake.requests()[0].body);
    EXPECT_EQ(body["name"], "design discussion");
    EXPECT_TRUE(body.contains("auto_archive_duration"));
}

TEST(DiscordThreads, AddReactionHitsReactionsEndpoint) {
    FakeHttpTransport fake;
    fake.enqueue_response({204, "", {}});
    DiscordAdapter::Config cfg;
    cfg.bot_token = "TKN";
    DiscordAdapter adapter(cfg, &fake);

    EXPECT_TRUE(adapter.add_reaction("111", "222", "👍"));
    ASSERT_EQ(fake.requests().size(), 1u);
    auto& url = fake.requests()[0].url;
    EXPECT_NE(url.find("/channels/111/messages/222/reactions/"),
              std::string::npos);
    EXPECT_NE(url.find("/@me"), std::string::npos);
}

TEST(DiscordThreads, EmojiIsUrlEncoded) {
    FakeHttpTransport fake;
    fake.enqueue_response({204, "", {}});
    DiscordAdapter::Config cfg;
    cfg.bot_token = "TKN";
    DiscordAdapter adapter(cfg, &fake);

    EXPECT_TRUE(adapter.add_reaction("111", "222", "👍"));
    auto& url = fake.requests()[0].url;
    // Raw UTF-8 thumbs-up byte sequence should NOT appear un-encoded.
    EXPECT_EQ(url.find("\xF0\x9F\x91\x8D"), std::string::npos);
    // Should instead be percent-encoded.
    EXPECT_NE(url.find("%F0%9F%91%8D"), std::string::npos);
}
