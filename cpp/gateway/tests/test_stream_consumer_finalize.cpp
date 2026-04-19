// Tests for the REQUIRES_EDIT_FINALIZE capability + finalize kwarg.
//
// Upstream 4459913f: adapters with distinct streaming-UI in-progress
// state (e.g. DingTalk AI Cards) must receive an explicit finalize
// edit at stream end, even when content is identical to the last edit.
// Adapters without the flag treat finalize as a no-op.
#include <gtest/gtest.h>

#include <hermes/gateway/stream_consumer.hpp>

#include <string>
#include <vector>

namespace hg = hermes::gateway;

namespace {

struct Edit {
    std::string chat;
    std::string mid;
    std::string content;
    bool finalize;
};

}  // namespace

TEST(StreamConsumerFinalize, DefaultCapabilityIsFalse) {
    std::vector<Edit> edits;
    hg::StreamConsumer consumer([&](const std::string& c,
                                      const std::string& m,
                                      const std::string& s, bool f) {
        edits.push_back({c, m, s, f});
    });
    EXPECT_FALSE(consumer.requires_edit_finalize());
}

TEST(StreamConsumerFinalize, FinalizeFlagPropagatesOnFlush) {
    // With new content at flush time the final edit fires with
    // finalize=true regardless of the capability flag.
    std::vector<Edit> edits;
    hg::StreamConsumer consumer([&](const std::string& c,
                                      const std::string& m,
                                      const std::string& s, bool f) {
        edits.push_back({c, m, s, f});
    });

    consumer.feed_token("chat", "mid", "hello");
    consumer.feed_token("chat", "mid", " world");  // batched
    consumer.flush("chat", "mid", /*finalize=*/true);

    ASSERT_FALSE(edits.empty());
    EXPECT_TRUE(edits.back().finalize);
    EXPECT_EQ(edits.back().content, "hello world");
}

TEST(StreamConsumerFinalize, FlushNonFinalizeMatchesLegacyBehavior) {
    std::vector<Edit> edits;
    hg::StreamConsumer consumer([&](const std::string& c,
                                      const std::string& m,
                                      const std::string& s, bool f) {
        edits.push_back({c, m, s, f});
    });

    consumer.feed_token("chat", "mid", "x");
    consumer.flush("chat", "mid");  // default finalize=false
    ASSERT_FALSE(edits.empty());
    EXPECT_FALSE(edits.back().finalize);
}

TEST(StreamConsumerFinalize, IdenticalContentSkipsWhenCapabilityOff) {
    // With REQUIRES_EDIT_FINALIZE=false (default), a finalize call
    // with content identical to the previous edit is a no-op.
    int calls = 0;
    hg::StreamConsumer consumer([&](const std::string&, const std::string&,
                                      const std::string&, bool) { ++calls; });

    consumer.feed_token("chat", "mid", "abc");
    int after_feed = calls;
    consumer.flush("chat", "mid", /*finalize=*/true);
    // Content "abc" was already sent on the immediate-flush in feed_token;
    // the subsequent finalize with same content should be skipped.
    EXPECT_EQ(calls, after_feed);
}

TEST(StreamConsumerFinalize, IdenticalContentFiresWhenCapabilityOn) {
    // With REQUIRES_EDIT_FINALIZE=true, a finalize call with identical
    // content MUST fire the callback so the streaming UI transitions
    // out of the in-progress state.
    std::vector<Edit> edits;
    hg::StreamConsumer::AdapterCapabilities caps;
    caps.requires_edit_finalize = true;
    hg::StreamConsumer consumer(
        [&](const std::string& c, const std::string& m,
             const std::string& s, bool f) {
            edits.push_back({c, m, s, f});
        },
        caps);

    consumer.feed_token("chat", "mid", "abc");
    int before = static_cast<int>(edits.size());
    consumer.flush("chat", "mid", /*finalize=*/true);
    ASSERT_GT(static_cast<int>(edits.size()), before);
    EXPECT_TRUE(edits.back().finalize);
    EXPECT_EQ(edits.back().content, "abc");
}

TEST(StreamConsumerFinalize, SetCapabilitiesAtRuntime) {
    hg::StreamConsumer consumer(
        [](const std::string&, const std::string&, const std::string&,
            bool) {});
    EXPECT_FALSE(consumer.requires_edit_finalize());
    hg::StreamConsumer::AdapterCapabilities caps;
    caps.requires_edit_finalize = true;
    consumer.set_adapter_capabilities(caps);
    EXPECT_TRUE(consumer.requires_edit_finalize());
}

TEST(StreamConsumerFinalize, FlushAllPropagatesFinalize) {
    std::vector<Edit> edits;
    hg::StreamConsumer consumer([&](const std::string& c,
                                      const std::string& m,
                                      const std::string& s, bool f) {
        edits.push_back({c, m, s, f});
    });

    consumer.feed_token("A", "m1", "1");
    consumer.feed_token("B", "m2", "2");
    // Note: each feed_token fires an immediate edit (no prior last_edit),
    // so the buffers are already flushed.  flush_all on new content
    // would fire; without new content it's a no-op.  To exercise the
    // finalize path, we add another token and then flush_all(true).
    consumer.feed_token("A", "m1", "MORE");
    // Immediate re-flush is batched; flush_all forces it.
    edits.clear();
    consumer.flush_all(/*finalize=*/true);

    // At least one edit should carry finalize=true.
    bool saw_finalize = false;
    for (auto& e : edits) {
        if (e.finalize) saw_finalize = true;
    }
    EXPECT_TRUE(saw_finalize);
}
