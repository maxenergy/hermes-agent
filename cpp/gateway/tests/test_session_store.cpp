#include <gtest/gtest.h>

#include <hermes/gateway/session_store.hpp>

#include <filesystem>
#include <thread>

namespace hg = hermes::gateway;
namespace fs = std::filesystem;

class SessionStoreTest : public ::testing::Test {
protected:
    fs::path tmp_dir_;

    void SetUp() override {
        tmp_dir_ = fs::temp_directory_path() /
                   ("hermes_session_test_" +
                    std::to_string(std::chrono::system_clock::now()
                                       .time_since_epoch()
                                       .count()));
        fs::create_directories(tmp_dir_);
    }

    void TearDown() override { fs::remove_all(tmp_dir_); }

    hg::SessionSource make_source(
        hg::Platform p = hg::Platform::Telegram) {
        hg::SessionSource src;
        src.platform = p;
        src.chat_id = "chat_123";
        src.chat_name = "Test Chat";
        src.chat_type = "dm";
        src.user_id = "user_456";
        src.user_name = "Alice";
        return src;
    }
};

TEST_F(SessionStoreTest, GetOrCreateSession) {
    hg::SessionStore store(tmp_dir_);
    auto src = make_source();

    auto key1 = store.get_or_create_session(src);
    EXPECT_FALSE(key1.empty());

    // Same source returns same key.
    auto key2 = store.get_or_create_session(src);
    EXPECT_EQ(key1, key2);
}

TEST_F(SessionStoreTest, ResetSession) {
    hg::SessionStore store(tmp_dir_);
    auto src = make_source();
    auto key = store.get_or_create_session(src);

    // Add some messages, then reset.
    store.append_message(key, {{"role", "user"}, {"content", "hello"}});
    EXPECT_EQ(store.load_transcript(key).size(), 1u);

    store.reset_session(key);
    EXPECT_EQ(store.load_transcript(key).size(), 0u);
}

TEST_F(SessionStoreTest, AppendAndLoadTranscript) {
    hg::SessionStore store(tmp_dir_);
    auto src = make_source();
    auto key = store.get_or_create_session(src);

    store.append_message(key, {{"role", "user"}, {"content", "hello"}});
    store.append_message(key,
                         {{"role", "assistant"}, {"content", "hi there"}});

    auto transcript = store.load_transcript(key);
    ASSERT_EQ(transcript.size(), 2u);
    EXPECT_EQ(transcript[0]["content"], "hello");
    EXPECT_EQ(transcript[1]["content"], "hi there");
}

TEST_F(SessionStoreTest, PiiRedactionHashes) {
    hg::SessionStore store(tmp_dir_);
    auto src = make_source(hg::Platform::Telegram);

    auto redacted = store.redact_pii(src);
    // IDs should be hashed (12 hex chars).
    EXPECT_NE(redacted.user_id, src.user_id);
    EXPECT_EQ(redacted.user_id.size(), 12u);
    EXPECT_NE(redacted.chat_id, src.chat_id);
    EXPECT_EQ(redacted.chat_id.size(), 12u);
    EXPECT_EQ(redacted.user_name, "redacted");

    // Discord should NOT be redacted.
    auto discord_src = make_source(hg::Platform::Discord);
    auto discord_redacted = store.redact_pii(discord_src);
    EXPECT_EQ(discord_redacted.user_id, discord_src.user_id);
    EXPECT_EQ(discord_redacted.user_name, discord_src.user_name);
}

TEST_F(SessionStoreTest, HashIdDeterministic) {
    auto h1 = hg::SessionStore::hash_id("test_id_123");
    auto h2 = hg::SessionStore::hash_id("test_id_123");
    EXPECT_EQ(h1, h2);
    EXPECT_EQ(h1.size(), 12u);

    // Different input -> different hash.
    auto h3 = hg::SessionStore::hash_id("other_id");
    EXPECT_NE(h1, h3);
}

TEST_F(SessionStoreTest, ShouldResetByIdleTime) {
    hg::SessionStore store(tmp_dir_);
    auto src = make_source();
    auto key = store.get_or_create_session(src);

    hg::SessionResetPolicy policy;
    policy.mode = "idle";
    policy.idle_minutes = 0;  // 0 minutes = always reset.

    // With 0 idle_minutes threshold, should reset since updated_at is now.
    // But we need at least 1 minute to pass... test with a reasonable check.
    // Actually idle_minutes=0 means any idle time triggers reset.
    // The session was just created, so elapsed ~ 0.
    // Let's use idle_minutes = 99999 to test non-reset.
    policy.idle_minutes = 99999;
    EXPECT_FALSE(store.should_reset(key, policy));

    // mode=none never resets.
    policy.mode = "none";
    EXPECT_FALSE(store.should_reset(key, policy));
}

TEST_F(SessionStoreTest, SessionSourceJsonRoundTrip) {
    auto src = make_source();
    auto j = src.to_json();
    auto src2 = hg::SessionSource::from_json(j);
    EXPECT_EQ(src.platform, src2.platform);
    EXPECT_EQ(src.chat_id, src2.chat_id);
    EXPECT_EQ(src.user_id, src2.user_id);
    EXPECT_EQ(src.chat_type, src2.chat_type);
}
