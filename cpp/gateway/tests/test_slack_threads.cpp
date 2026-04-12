// Tests for Slack thread detection and file uploads.
#include <gtest/gtest.h>

#include "../platforms/slack.hpp"

#include <hermes/llm/llm_client.hpp>
#include <nlohmann/json.hpp>

using hermes::gateway::platforms::SlackAdapter;
using hermes::llm::FakeHttpTransport;

TEST(SlackThreads, ParseThreadTsPresent) {
    nlohmann::json event = {{"thread_ts", "1710000000.000100"}};
    auto ts = SlackAdapter::parse_thread_ts(event);
    ASSERT_TRUE(ts.has_value());
    EXPECT_EQ(*ts, "1710000000.000100");
}

TEST(SlackThreads, ParseThreadTsAbsent) {
    auto ts = SlackAdapter::parse_thread_ts(nlohmann::json::object());
    EXPECT_FALSE(ts.has_value());
}

TEST(SlackThreads, SendThreadReplyIncludesThreadTs) {
    FakeHttpTransport fake;
    fake.enqueue_response({200, R"({"ok":true})", {}});
    SlackAdapter::Config cfg;
    cfg.bot_token = "xoxb-1";
    SlackAdapter adapter(cfg, &fake);

    EXPECT_TRUE(
        adapter.send_thread_reply("C123", "1710000000.000100", "hi"));
    ASSERT_EQ(fake.requests().size(), 1u);
    auto body = nlohmann::json::parse(fake.requests()[0].body);
    EXPECT_EQ(body["channel"], "C123");
    EXPECT_EQ(body["thread_ts"], "1710000000.000100");
    EXPECT_EQ(body["text"], "hi");
}

TEST(SlackThreads, UploadFilePostsToFilesUpload) {
    FakeHttpTransport fake;
    fake.enqueue_response({200, R"({"ok":true})", {}});
    SlackAdapter::Config cfg;
    cfg.bot_token = "xoxb-1";
    SlackAdapter adapter(cfg, &fake);

    EXPECT_TRUE(adapter.upload_file("C123", "notes.txt", "hello"));
    ASSERT_EQ(fake.requests().size(), 1u);
    EXPECT_NE(fake.requests()[0].url.find("files.upload"), std::string::npos);
    auto body = nlohmann::json::parse(fake.requests()[0].body);
    EXPECT_EQ(body["channels"], "C123");
    EXPECT_EQ(body["filename"], "notes.txt");
    EXPECT_EQ(body["content"], "hello");
}
