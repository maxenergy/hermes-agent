// Tests for SessionEntry.resume_pending + drain-timeout auto-resume.
//
// Ports tests/gateway/test_restart_resume_pending.py (upstream cb4addac,
// c49a58a6) to the C++ SessionStore surface.
#include <gtest/gtest.h>

#include <hermes/gateway/session_manager.hpp>
#include <hermes/gateway/session_store.hpp>

#include <filesystem>

namespace hg = hermes::gateway;
namespace fs = std::filesystem;

class SessionResumePendingTest : public ::testing::Test {
protected:
    fs::path tmp_dir_;

    void SetUp() override {
        tmp_dir_ = fs::temp_directory_path() /
                   ("hermes_resume_pending_test_" +
                    std::to_string(std::chrono::system_clock::now()
                                       .time_since_epoch()
                                       .count()));
        fs::create_directories(tmp_dir_);
    }

    void TearDown() override { fs::remove_all(tmp_dir_); }

    hg::SessionSource make_source() {
        hg::SessionSource src;
        src.platform = hg::Platform::Telegram;
        src.chat_id = "chat_123";
        src.user_id = "user_456";
        src.chat_type = "dm";
        return src;
    }
};

// mark_resume_pending requires an existing session; returns false otherwise.
TEST_F(SessionResumePendingTest, MarkRequiresExistingSession) {
    hg::SessionStore store(tmp_dir_);
    EXPECT_FALSE(store.mark_resume_pending("nonexistent:key"));
}

// Mark + query.
TEST_F(SessionResumePendingTest, MarkAndQuery) {
    hg::SessionStore store(tmp_dir_);
    auto key = store.get_or_create_session(make_source());
    EXPECT_FALSE(store.is_resume_pending(key));
    EXPECT_TRUE(store.mark_resume_pending(key, "restart_timeout"));
    EXPECT_TRUE(store.is_resume_pending(key));
    EXPECT_EQ(store.resume_reason(key), "restart_timeout");
}

// Drain-timeout pattern: mark active sessions resume_pending, then on the
// next inbound message the transcript survives and the session_id is
// preserved.  consume_resume_pending returns should_resume=true once.
TEST_F(SessionResumePendingTest, DrainMarkThenNextMessageResumes) {
    hg::SessionStore store(tmp_dir_);
    auto key = store.get_or_create_session(make_source());
    store.append_message(key, {{"role", "user"}, {"content", "hello"}});
    store.append_message(key, {{"role", "assistant"}, {"content", "hi"}});
    ASSERT_EQ(store.load_transcript(key).size(), 2u);

    // Simulate drain-timeout path: mark the session resumable.
    EXPECT_TRUE(store.mark_resume_pending(key, "restart_timeout"));

    // Next inbound message on the same session_key: transcript + id
    // survive, and consume_resume_pending returns should_resume=true.
    auto key2 = store.get_or_create_session(make_source());
    EXPECT_EQ(key, key2);
    EXPECT_EQ(store.load_transcript(key).size(), 2u);

    auto d = store.consume_resume_pending(key);
    EXPECT_TRUE(d.should_resume);
    EXPECT_EQ(d.reason, "restart_timeout");
    EXPECT_FALSE(d.escalated_to_suspended);
}

// Successful resume clears the flag + counter.
TEST_F(SessionResumePendingTest, SuccessfulResumeClearsCounter) {
    hg::SessionStore store(tmp_dir_);
    auto key = store.get_or_create_session(make_source());
    ASSERT_TRUE(store.mark_resume_pending(key));
    auto d = store.consume_resume_pending(key);
    EXPECT_TRUE(d.should_resume);
    EXPECT_EQ(store.restart_failure_count(key), 1);  // bumped optimistically

    // Runner calls clear after a clean turn.
    EXPECT_TRUE(store.clear_resume_pending(key));
    EXPECT_FALSE(store.is_resume_pending(key));
    EXPECT_EQ(store.restart_failure_count(key), 0);
}

// Stuck-loop escalation: after kResumeFailureThreshold attempts, the next
// consume promotes to suspended and wipes the transcript.
TEST_F(SessionResumePendingTest, StuckLoopEscalationToSuspended) {
    hg::SessionStore store(tmp_dir_);
    auto key = store.get_or_create_session(make_source());
    store.append_message(key, {{"role", "user"}, {"content", "stuck"}});

    // Simulate 3 failed resume attempts: mark -> consume (counter bumps)
    // but never clear, mark again, repeat.
    for (int i = 0; i < hg::SessionStore::kResumeFailureThreshold; ++i) {
        ASSERT_TRUE(store.mark_resume_pending(key));
        auto d = store.consume_resume_pending(key);
        EXPECT_TRUE(d.should_resume) << "attempt " << i;
        EXPECT_FALSE(d.escalated_to_suspended) << "attempt " << i;
    }
    // Fourth attempt escalates.
    ASSERT_TRUE(store.mark_resume_pending(key));
    auto final_decision = store.consume_resume_pending(key);
    EXPECT_FALSE(final_decision.should_resume);
    EXPECT_TRUE(final_decision.escalated_to_suspended);
    EXPECT_TRUE(store.is_suspended(key));
    // Transcript wiped.
    EXPECT_EQ(store.load_transcript(key).size(), 0u);
    // Counter reset post-escalation.
    EXPECT_EQ(store.restart_failure_count(key), 0);
}

// suspended wins over resume_pending in get_or_create_session (cb4addac).
TEST_F(SessionResumePendingTest, SuspendedWinsOverResumePending) {
    hg::SessionStore store(tmp_dir_);
    auto key = store.get_or_create_session(make_source());
    store.append_message(key, {{"role", "user"}, {"content", "old"}});

    // Manually mark resume_pending + suspended simultaneously — not a
    // legal state in production, but we want the get_or_create_session
    // path to pick the forced-wipe branch unambiguously.
    ASSERT_TRUE(store.mark_resume_pending(key));
    // Bump counter past threshold + consume -> suspended.
    for (int i = 0; i <= hg::SessionStore::kResumeFailureThreshold; ++i) {
        store.mark_resume_pending(key);
        store.consume_resume_pending(key);
    }
    ASSERT_TRUE(store.is_suspended(key));

    // Next get_or_create_session wipes the transcript.
    auto key2 = store.get_or_create_session(make_source());
    EXPECT_EQ(key, key2);
    EXPECT_EQ(store.load_transcript(key).size(), 0u);
    EXPECT_FALSE(store.is_suspended(key));
}

// mark_resume_pending refuses to override suspended.
TEST_F(SessionResumePendingTest, MarkRefusesSuspended) {
    hg::SessionStore store(tmp_dir_);
    auto key = store.get_or_create_session(make_source());

    // Force suspend via threshold escalation.
    for (int i = 0; i <= hg::SessionStore::kResumeFailureThreshold; ++i) {
        store.mark_resume_pending(key);
        store.consume_resume_pending(key);
    }
    ASSERT_TRUE(store.is_suspended(key));

    // Next access wipes suspended -> we need a still-suspended entry,
    // so re-suspend by reloading w/o get_or_create path: bump the
    // counter directly.
    // Easier: bring the entry back then try to mark while suspended.
    auto key2 = store.get_or_create_session(make_source());
    EXPECT_EQ(key, key2);
    EXPECT_FALSE(store.is_suspended(key));  // cleared by get_or_create

    // Manually set via mark + consume escalation again.
    for (int i = 0; i <= hg::SessionStore::kResumeFailureThreshold; ++i) {
        store.mark_resume_pending(key);
        store.consume_resume_pending(key);
    }
    ASSERT_TRUE(store.is_suspended(key));
    // Now attempt to mark — must be refused since suspended.
    EXPECT_FALSE(store.mark_resume_pending(key));
    EXPECT_FALSE(store.is_resume_pending(key));
}

// c49a58a6: drain-timeout path should only mark still-running sessions.
// We expose this as "mark only the keys the runner reports as still
// blocking shutdown".  The test here exercises mark_resume_pending on a
// subset of session_keys and verifies the non-marked ones remain clean.
TEST_F(SessionResumePendingTest, DrainMarkIsPerKeyFiltering) {
    hg::SessionStore store(tmp_dir_);
    hg::SessionSource a = make_source();
    a.chat_id = "finisher";
    hg::SessionSource b = make_source();
    b.chat_id = "still_running";

    auto key_a = store.get_or_create_session(a);
    auto key_b = store.get_or_create_session(b);

    // Only B is still blocking shutdown at drain timeout.  The runner
    // iterates ``_running_agents`` at timeout time and marks only those.
    EXPECT_TRUE(store.mark_resume_pending(key_b, "restart_timeout"));

    EXPECT_FALSE(store.is_resume_pending(key_a));  // finisher not marked
    EXPECT_TRUE(store.is_resume_pending(key_b));
}

// SessionManager: mark_pending sentinel must not be treated as a real
// running agent for the drain-timeout mark loop.  The runner filters
// them out — we mirror that by exposing is_agent_pending.
TEST_F(SessionResumePendingTest, SessionManagerPendingSentinelSkipped) {
    hg::SessionManager mgr;
    ASSERT_TRUE(mgr.mark_pending("sess:pending"));
    EXPECT_TRUE(mgr.is_agent_pending("sess:pending"));
    // Pending sentinel sessions should NOT appear in a drain-timeout
    // mark loop.  The snapshot API lets the caller filter on is_pending.
    auto snapshot = mgr.snapshot_running();
    int real_running = 0;
    for (auto& e : snapshot) {
        if (!e.is_pending) ++real_running;
    }
    EXPECT_EQ(real_running, 0);
}
