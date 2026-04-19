// Tests for SessionStore::prune_old_entries + SessionStore::close +
// ShutdownSequencer wiring.
//
// Ports tests/gateway/test_session_store_prune.py (upstream eb07c056)
// and the shutdown-close half of test_session_state_cleanup.py
// (upstream 31e72764) to the C++ surface.
#include <gtest/gtest.h>

#include <hermes/gateway/session_store.hpp>
#include <hermes/gateway/shutdown_sequencer.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <thread>

namespace hg = hermes::gateway;
namespace fs = std::filesystem;

class SessionStorePruneTest : public ::testing::Test {
protected:
    fs::path tmp_dir_;

    void SetUp() override {
        tmp_dir_ = fs::temp_directory_path() /
                   ("hermes_prune_test_" +
                    std::to_string(std::chrono::system_clock::now()
                                       .time_since_epoch()
                                       .count()));
        fs::create_directories(tmp_dir_);
    }

    void TearDown() override { fs::remove_all(tmp_dir_); }

    hg::SessionSource src(const std::string& chat_id) {
        hg::SessionSource s;
        s.platform = hg::Platform::Telegram;
        s.chat_id = chat_id;
        s.user_id = "u";
        s.chat_type = "dm";
        return s;
    }

    // Rewrite an existing session.json to age its updated_at field by
    // N days (used so we can exercise the cutoff without waiting).
    void age_session(const std::string& session_key, int days_old) {
        auto path = tmp_dir_ / session_key / "session.json";
        std::ifstream in(path);
        nlohmann::json j;
        in >> j;
        in.close();
        auto old_time = std::chrono::system_clock::now() -
                        std::chrono::hours(24 * days_old);
        auto t = std::chrono::system_clock::to_time_t(old_time);
        std::tm tm{};
        gmtime_r(&t, &tm);
        char buf[32];
        std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
        j["updated_at"] = std::string(buf);
        std::ofstream out(path);
        out << j.dump(2);
    }
};

TEST_F(SessionStorePruneTest, PruneDisabledByZeroOrNegative) {
    hg::SessionStore store(tmp_dir_);
    store.get_or_create_session(src("a"));
    EXPECT_EQ(store.prune_old_entries(0), 0u);
    EXPECT_EQ(store.prune_old_entries(-1), 0u);
}

TEST_F(SessionStorePruneTest, PruneDropsOldEntries) {
    hg::SessionStore store(tmp_dir_);
    auto key_old = store.get_or_create_session(src("old"));
    auto key_fresh = store.get_or_create_session(src("fresh"));
    age_session(key_old, /*days_old=*/100);

    std::size_t pruned = store.prune_old_entries(/*max_age_days=*/90);
    EXPECT_EQ(pruned, 1u);
    EXPECT_FALSE(fs::exists(tmp_dir_ / key_old));
    EXPECT_TRUE(fs::exists(tmp_dir_ / key_fresh));
}

TEST_F(SessionStorePruneTest, PruneSkipsSuspendedEntries) {
    hg::SessionStore store(tmp_dir_);
    auto key = store.get_or_create_session(src("suspended"));
    age_session(key, /*days_old=*/200);

    // Directly mark as suspended in the session.json.
    auto path = tmp_dir_ / key / "session.json";
    std::ifstream in(path);
    nlohmann::json j;
    in >> j;
    in.close();
    j["suspended"] = true;
    std::ofstream out(path);
    out << j.dump(2);
    out.close();

    std::size_t pruned = store.prune_old_entries(/*max_age_days=*/90);
    EXPECT_EQ(pruned, 0u);
    EXPECT_TRUE(fs::exists(tmp_dir_ / key));
}

TEST_F(SessionStorePruneTest, PruneUsesUpdatedAtNotCreatedAt) {
    hg::SessionStore store(tmp_dir_);
    auto key = store.get_or_create_session(src("a"));
    age_session(key, /*days_old=*/95);
    // Touch: re-access updates updated_at.
    store.get_or_create_session(src("a"));

    std::size_t pruned = store.prune_old_entries(/*max_age_days=*/90);
    EXPECT_EQ(pruned, 0u);
    EXPECT_TRUE(fs::exists(tmp_dir_ / key));
}

TEST_F(SessionStorePruneTest, PruneCleansOrphanCounters) {
    hg::SessionStore store(tmp_dir_);
    auto key = store.get_or_create_session(src("a"));
    // Create a counter for the session, then age + prune it.
    store.bump_restart_failure_count(key);
    EXPECT_EQ(store.restart_failure_count(key), 1);
    age_session(key, /*days_old=*/200);

    std::size_t pruned = store.prune_old_entries(/*max_age_days=*/90);
    EXPECT_EQ(pruned, 1u);
    // Counter for the removed session should also be gone.
    EXPECT_EQ(store.restart_failure_count(key), 0);
}

TEST_F(SessionStorePruneTest, CloseIsIdempotent) {
    hg::SessionStore store(tmp_dir_);
    auto key = store.get_or_create_session(src("a"));
    store.close();
    store.close();  // second call is a no-op
    // Other accessors still function after close().
    EXPECT_NO_THROW(store.get_or_create_session(src("a")));
    EXPECT_EQ(store.load_transcript(key).size(), 0u);
}

TEST(ShutdownSequencerPrune, InvokesPruneAndCloseInFlushPhase) {
    hg::ShutdownSequencer seq;

    int close_calls = 0;
    std::size_t prune_returned = 7;

    seq.set_session_flush([] {});
    seq.set_session_close([&] { ++close_calls; });
    seq.set_stale_prune([&] { return prune_returned; });

    hg::ShutdownBudget b;
    b.drain_grace = std::chrono::milliseconds(0);
    b.agent_drain_timeout = std::chrono::milliseconds(10);
    b.per_adapter_timeout = std::chrono::milliseconds(100);
    b.session_flush_timeout = std::chrono::milliseconds(500);
    seq.set_budget(b);

    auto out = seq.run("test");
    EXPECT_EQ(close_calls, 1);
    EXPECT_EQ(out.stale_pruned, prune_returned);
    EXPECT_TRUE(out.session_closed);
    EXPECT_EQ(out.final_phase, hg::ShutdownPhase::Stopped);
}

TEST(ShutdownSequencerPrune, HandlesPruneException) {
    hg::ShutdownSequencer seq;
    seq.set_session_flush([] {});
    seq.set_stale_prune([&]() -> std::size_t {
        throw std::runtime_error("boom");
    });

    hg::ShutdownBudget b;
    b.drain_grace = std::chrono::milliseconds(0);
    b.agent_drain_timeout = std::chrono::milliseconds(10);
    b.session_flush_timeout = std::chrono::milliseconds(500);
    seq.set_budget(b);

    auto out = seq.run();
    EXPECT_EQ(out.stale_pruned, 0u);
    bool saw = false;
    for (auto& e : out.errors) {
        if (e.find("stale_prune") != std::string::npos) saw = true;
    }
    EXPECT_TRUE(saw);
}

TEST(ShutdownSequencerPrune, NoPruneFnIsNoop) {
    hg::ShutdownSequencer seq;
    seq.set_session_flush([] {});
    hg::ShutdownBudget b;
    b.drain_grace = std::chrono::milliseconds(0);
    b.agent_drain_timeout = std::chrono::milliseconds(10);
    b.session_flush_timeout = std::chrono::milliseconds(100);
    seq.set_budget(b);
    auto out = seq.run();
    EXPECT_EQ(out.stale_pruned, 0u);
    EXPECT_FALSE(out.session_closed);
}
