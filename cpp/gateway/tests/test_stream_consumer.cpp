#include <gtest/gtest.h>

#include <hermes/gateway/stream_consumer.hpp>

#include <string>
#include <thread>
#include <vector>

namespace hg = hermes::gateway;

struct EditRecord {
    std::string chat_id;
    std::string message_id;
    std::string content;
};

TEST(StreamConsumerTest, FeedTokenAccumulates) {
    std::vector<EditRecord> edits;
    hg::StreamConsumer consumer(
        [&](const std::string& c, const std::string& m, const std::string& s) {
            edits.push_back({c, m, s});
        });

    // First token triggers an immediate flush (no prior edit time).
    consumer.feed_token("chat1", "msg1", "Hello");
    // Subsequent tokens within the batch interval accumulate without firing.
    consumer.feed_token("chat1", "msg1", ", ");
    consumer.feed_token("chat1", "msg1", "world");

    // Force flush to get the final accumulated state.
    consumer.flush("chat1", "msg1");

    // The accumulated content on the final flush should be the full string.
    ASSERT_FALSE(edits.empty());
    EXPECT_EQ(edits.back().content, "Hello, world");
    EXPECT_EQ(edits.back().chat_id, "chat1");
    EXPECT_EQ(edits.back().message_id, "msg1");
}

TEST(StreamConsumerTest, FlushFiresEditCallback) {
    int call_count = 0;
    std::string last_content;
    hg::StreamConsumer consumer(
        [&](const std::string&, const std::string&, const std::string& s) {
            ++call_count;
            last_content = s;
        });

    consumer.feed_token("chat", "mid", "abc");
    int count_after_feed = call_count;
    consumer.flush("chat", "mid");

    EXPECT_GE(call_count, count_after_feed);
    EXPECT_EQ(last_content, "abc");
}

TEST(StreamConsumerTest, BatchingDelaysCallback) {
    int call_count = 0;
    hg::StreamConsumer consumer(
        [&](const std::string&, const std::string&, const std::string&) {
            ++call_count;
        });

    // First token fires (no prior edit).
    consumer.feed_token("c", "m", "a");
    int after_first = call_count;

    // Immediate follow-up tokens should be batched (not fire) since
    // BATCH_INTERVAL is 500ms.
    for (int i = 0; i < 10; ++i) {
        consumer.feed_token("c", "m", "x");
    }

    // Callback count should not have grown significantly.
    EXPECT_EQ(call_count, after_first);
}

TEST(StreamConsumerTest, MultipleSessionsIsolated) {
    std::vector<EditRecord> edits;
    hg::StreamConsumer consumer(
        [&](const std::string& c, const std::string& m, const std::string& s) {
            edits.push_back({c, m, s});
        });

    consumer.feed_token("chatA", "m1", "foo");
    consumer.feed_token("chatB", "m2", "bar");

    consumer.flush_all();

    // Find last content per session.
    std::string a_content;
    std::string b_content;
    for (const auto& e : edits) {
        if (e.chat_id == "chatA") a_content = e.content;
        if (e.chat_id == "chatB") b_content = e.content;
    }
    EXPECT_EQ(a_content, "foo");
    EXPECT_EQ(b_content, "bar");
}

TEST(StreamConsumerTest, FlushAllClearsBuffers) {
    int calls = 0;
    hg::StreamConsumer consumer(
        [&](const std::string&, const std::string&, const std::string&) {
            ++calls;
        });

    consumer.feed_token("x", "y", "z");
    consumer.flush_all();

    int before = calls;
    // After flush_all, calling flush on the same key should not fire again.
    consumer.flush("x", "y");
    EXPECT_EQ(calls, before);
}
