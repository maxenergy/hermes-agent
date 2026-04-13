// Joint integration tests — SessionDB persistence.

#include "hermes/state/session_db.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;
using hermes::state::FtsHit;
using hermes::state::MessageRow;
using hermes::state::SessionDB;
using hermes::state::SessionRow;

namespace {

fs::path unique_dir(const std::string& tag) {
    auto p = fs::temp_directory_path() /
             ("hermes_joint_session_" + tag + "_" +
              std::to_string(::getpid()) + "_" +
              std::to_string(std::chrono::system_clock::now()
                                 .time_since_epoch()
                                 .count()));
    fs::create_directories(p);
    return p;
}

}  // namespace

// 1. Messages persist across DB reopen.
TEST(JointSessionPersistence, MessagesSurviveReopen) {
    auto dir = unique_dir("reopen");
    auto db_path = dir / "sessions.db";

    std::string sid;
    {
        SessionDB db(db_path);
        sid = db.create_session("cli", "m1", nlohmann::json::object());
        for (int i = 0; i < 4; ++i) {
            MessageRow m;
            m.session_id = sid;
            m.turn_index = i;
            m.role = (i % 2) ? "assistant" : "user";
            m.content = "turn " + std::to_string(i);
            m.tool_calls = nlohmann::json::array();
            m.created_at = std::chrono::system_clock::now();
            db.save_message(m);
        }
        db.checkpoint();
    }

    // Reopen — the schema, session row, and messages should all survive.
    SessionDB db(db_path);
    auto got = db.get_session(sid);
    ASSERT_TRUE(got.has_value());
    auto msgs = db.get_messages(sid);
    ASSERT_EQ(msgs.size(), 4u);
    EXPECT_EQ(msgs[0].content, "turn 0");
    EXPECT_EQ(msgs[3].content, "turn 3");

    std::error_code ec;
    fs::remove_all(dir, ec);
}

// 2. FTS5 search finds historic content after reopen.
TEST(JointSessionPersistence, FtsSearchFindsHistoricContent) {
    auto dir = unique_dir("fts");
    auto db_path = dir / "sessions.db";

    {
        SessionDB db(db_path);
        auto sid = db.create_session("cli", "m1", nlohmann::json::object());
        MessageRow m;
        m.session_id = sid;
        m.turn_index = 0;
        m.role = "assistant";
        m.content = "The rare narwhal xyzzyvarble is found in polar seas.";
        m.tool_calls = nlohmann::json::array();
        m.created_at = std::chrono::system_clock::now();
        db.save_message(m);
        db.checkpoint();
    }

    SessionDB db(db_path);
    auto hits = db.fts_search("xyzzyvarble");
    ASSERT_FALSE(hits.empty());
    EXPECT_NE(hits.front().snippet.find("xyzzyvarble"), std::string::npos);

    std::error_code ec;
    fs::remove_all(dir, ec);
}

// 3. Concurrent writers — three threads × 50 messages on a single DB don't
// corrupt; total row count matches the sum.
TEST(JointSessionPersistence, ConcurrentWritesDoNotCorrupt) {
    auto dir = unique_dir("concur");
    auto db_path = dir / "sessions.db";

    SessionDB db(db_path);
    auto sid = db.create_session("cli", "m1", nlohmann::json::object());

    constexpr int kThreads = 3;
    constexpr int kPerThread = 50;
    std::vector<std::thread> ts;
    std::atomic<int> written{0};
    for (int t = 0; t < kThreads; ++t) {
        ts.emplace_back([&, t] {
            for (int i = 0; i < kPerThread; ++i) {
                MessageRow m;
                m.session_id = sid;
                m.turn_index = t * kPerThread + i;
                m.role = "user";
                m.content = "t" + std::to_string(t) + "-" + std::to_string(i);
                m.tool_calls = nlohmann::json::array();
                m.created_at = std::chrono::system_clock::now();
                db.save_message(m);
                written.fetch_add(1);
            }
        });
    }
    for (auto& th : ts) th.join();
    EXPECT_EQ(written.load(), kThreads * kPerThread);

    auto all = db.get_messages(sid);
    EXPECT_EQ(all.size(), static_cast<size_t>(kThreads * kPerThread));

    std::error_code ec;
    fs::remove_all(dir, ec);
}
