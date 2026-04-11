#include "hermes/state/session_db.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <random>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;
using hermes::state::MessageRow;
using hermes::state::SessionDB;
using hermes::state::SessionRow;

namespace {
fs::path make_tmpdir() {
    auto base = fs::temp_directory_path() /
                ("hermes_state_tests_" +
                 std::to_string(::getpid()) + "_" +
                 std::to_string(std::chrono::system_clock::now()
                                    .time_since_epoch()
                                    .count()));
    fs::create_directories(base);
    return base;
}
}  // namespace

class SessionDBTest : public ::testing::Test {
protected:
    fs::path dir_;
    fs::path db_path_;

    void SetUp() override {
        dir_ = make_tmpdir();
        db_path_ = dir_ / "sessions.db";
    }
    void TearDown() override {
        std::error_code ec;
        fs::remove_all(dir_, ec);
    }
};

TEST_F(SessionDBTest, OpenCreatesSchemaAtV6) {
    SessionDB db(db_path_);
    EXPECT_EQ(db.schema_version(), 6);
    EXPECT_TRUE(fs::exists(db_path_));
}

TEST_F(SessionDBTest, CreateAndGetSessionRoundTrip) {
    SessionDB db(db_path_);
    nlohmann::json cfg = {{"temperature", 0.2}, {"top_p", 0.9}};
    auto id = db.create_session("cli", "qwen3-coder", cfg);
    ASSERT_FALSE(id.empty());

    auto fetched = db.get_session(id);
    ASSERT_TRUE(fetched.has_value());
    EXPECT_EQ(fetched->id, id);
    EXPECT_EQ(fetched->source, "cli");
    EXPECT_EQ(fetched->model, "qwen3-coder");
    EXPECT_EQ(fetched->config["temperature"], 0.2);
    EXPECT_EQ(fetched->input_tokens, 0);
    EXPECT_EQ(fetched->output_tokens, 0);
    EXPECT_DOUBLE_EQ(fetched->cost_usd, 0.0);
}

TEST_F(SessionDBTest, SaveAndGetMessagesPreservesOrder) {
    SessionDB db(db_path_);
    auto id = db.create_session("cli", "qwen3", nlohmann::json::object());

    for (int i = 0; i < 5; ++i) {
        MessageRow msg;
        msg.session_id = id;
        msg.turn_index = i;
        msg.role = (i % 2 == 0) ? "user" : "assistant";
        msg.content = "message #" + std::to_string(i);
        msg.tool_calls = nlohmann::json::array();
        msg.created_at =
            std::chrono::system_clock::now() + std::chrono::milliseconds(i);
        db.save_message(msg);
    }

    auto msgs = db.get_messages(id);
    ASSERT_EQ(msgs.size(), 5u);
    for (int i = 0; i < 5; ++i) {
        EXPECT_EQ(msgs[static_cast<std::size_t>(i)].turn_index, i);
        EXPECT_EQ(msgs[static_cast<std::size_t>(i)].content,
                  "message #" + std::to_string(i));
    }
}

TEST_F(SessionDBTest, FtsSearchFindsInsertedContent) {
    SessionDB db(db_path_);
    auto id = db.create_session("cli", "qwen3", nlohmann::json::object());

    MessageRow msg;
    msg.session_id = id;
    msg.turn_index = 0;
    msg.role = "user";
    msg.content =
        "how do I implement a retry loop with jittered backoff in cpp";
    msg.tool_calls = nlohmann::json::array();
    db.save_message(msg);

    MessageRow msg2 = msg;
    msg2.turn_index = 1;
    msg2.role = "assistant";
    msg2.content = "use hermes::core::retry::jittered_backoff";
    db.save_message(msg2);

    auto hits = db.fts_search("jittered", 10);
    EXPECT_GE(hits.size(), 1u);
    bool found_session = false;
    for (const auto& h : hits) {
        if (h.session_id == id) found_session = true;
    }
    EXPECT_TRUE(found_session);
}

TEST_F(SessionDBTest, AddTokensAccumulates) {
    SessionDB db(db_path_);
    auto id = db.create_session("cli", "qwen3", nlohmann::json::object());
    db.add_tokens(id, 100, 50, 0.002);
    db.add_tokens(id, 20, 10, 0.0005);

    auto fetched = db.get_session(id);
    ASSERT_TRUE(fetched);
    EXPECT_EQ(fetched->input_tokens, 120);
    EXPECT_EQ(fetched->output_tokens, 60);
    EXPECT_NEAR(fetched->cost_usd, 0.0025, 1e-9);
}

TEST_F(SessionDBTest, ListSessionsHonorsLimitOffset) {
    SessionDB db(db_path_);
    std::vector<std::string> ids;
    for (int i = 0; i < 5; ++i) {
        ids.push_back(
            db.create_session("cli", "m", nlohmann::json::object()));
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }

    auto first_two = db.list_sessions(2, 0);
    EXPECT_EQ(first_two.size(), 2u);

    auto next_two = db.list_sessions(2, 2);
    EXPECT_EQ(next_two.size(), 2u);

    auto remainder = db.list_sessions(2, 4);
    EXPECT_EQ(remainder.size(), 1u);
}

TEST_F(SessionDBTest, DeleteSessionCascadesMessages) {
    SessionDB db(db_path_);
    auto id = db.create_session("cli", "qwen", nlohmann::json::object());

    MessageRow msg;
    msg.session_id = id;
    msg.turn_index = 0;
    msg.role = "user";
    msg.content = "hello";
    msg.tool_calls = nlohmann::json::array();
    db.save_message(msg);

    EXPECT_EQ(db.get_messages(id).size(), 1u);
    db.delete_session(id);
    EXPECT_FALSE(db.get_session(id).has_value());
    EXPECT_EQ(db.get_messages(id).size(), 0u);
}

TEST_F(SessionDBTest, ConcurrentWritersDoNotDeadlock) {
    SessionDB db(db_path_);
    std::atomic<int> ok{0};
    auto worker = [&]() {
        for (int i = 0; i < 50; ++i) {
            auto id =
                db.create_session("cli", "m", nlohmann::json::object());
            MessageRow msg;
            msg.session_id = id;
            msg.turn_index = 0;
            msg.role = "user";
            msg.content = "content " + std::to_string(i);
            msg.tool_calls = nlohmann::json::array();
            db.save_message(msg);
            ok.fetch_add(1);
        }
    };
    std::thread t1(worker);
    std::thread t2(worker);
    t1.join();
    t2.join();
    EXPECT_EQ(ok.load(), 100);
    auto rows = db.list_sessions(1000, 0);
    EXPECT_EQ(rows.size(), 100u);
}

TEST_F(SessionDBTest, CheckpointDoesNotThrow) {
    SessionDB db(db_path_);
    for (int i = 0; i < 10; ++i) {
        auto id =
            db.create_session("cli", "m", nlohmann::json::object());
        MessageRow msg;
        msg.session_id = id;
        msg.turn_index = 0;
        msg.role = "user";
        msg.content = "x";
        msg.tool_calls = nlohmann::json::array();
        db.save_message(msg);
    }
    EXPECT_NO_THROW(db.checkpoint());
}
