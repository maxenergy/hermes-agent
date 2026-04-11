#include "hermes/state/process_registry.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <string>
#include <thread>
#include <unistd.h>

namespace fs = std::filesystem;
using hermes::state::ProcessRegistry;
using hermes::state::ProcessSession;
using hermes::state::ProcessState;

namespace {
fs::path make_tmpdir() {
    auto base = fs::temp_directory_path() /
                ("hermes_proc_tests_" +
                 std::to_string(::getpid()) + "_" +
                 std::to_string(std::chrono::system_clock::now()
                                    .time_since_epoch()
                                    .count()));
    fs::create_directories(base);
    return base;
}

ProcessSession make_session(const std::string& id) {
    ProcessSession s;
    s.id = id;
    s.command = "sleep 1";
    s.task_id = "task-1";
    s.session_key = "sess-1";
    s.pid = 42;
    s.cwd = "/tmp";
    s.started_at = std::chrono::system_clock::now();
    s.updated_at = s.started_at;
    s.state = ProcessState::Running;
    return s;
}
}  // namespace

class ProcessRegistryTest : public ::testing::Test {
protected:
    fs::path dir_;
    fs::path cp_;
    void SetUp() override {
        dir_ = make_tmpdir();
        cp_ = dir_ / "processes.json";
    }
    void TearDown() override {
        std::error_code ec;
        fs::remove_all(dir_, ec);
    }
};

TEST_F(ProcessRegistryTest, RegisterAndGetRoundTrip) {
    ProcessRegistry reg(cp_);
    auto id = reg.register_process(make_session("p1"));
    ASSERT_EQ(id, "p1");
    auto fetched = reg.get(id);
    ASSERT_TRUE(fetched.has_value());
    EXPECT_EQ(fetched->command, "sleep 1");
    EXPECT_EQ(fetched->state, ProcessState::Running);
}

TEST_F(ProcessRegistryTest, AppendOutputRespectsRollingCap) {
    ProcessSession s = make_session("p1");
    s.output_buffer_max = 1024;
    std::string chunk(512, 'a');
    s.append_output(chunk);
    s.append_output(chunk);
    EXPECT_EQ(s.output_buffer.size(), 1024u);
    // Adding 100 more bytes should drop the oldest 100.
    std::string more(100, 'b');
    s.append_output(more);
    EXPECT_EQ(s.output_buffer.size(), 1024u);
    EXPECT_EQ(s.output_buffer.back(), 'b');
    // First 924 characters should still be 'a's.
    EXPECT_EQ(s.output_buffer.substr(0, 924), std::string(924, 'a'));
}

TEST_F(ProcessRegistryTest, Rolling200kDefault) {
    ProcessRegistry reg(cp_);
    auto id = reg.register_process(make_session("p1"));
    // Feed 300KB of data in 3 chunks.
    reg.feed_output(id, std::string(100 * 1024, 'x'));
    reg.feed_output(id, std::string(100 * 1024, 'y'));
    reg.feed_output(id, std::string(100 * 1024, 'z'));
    auto s = reg.get(id);
    ASSERT_TRUE(s);
    EXPECT_EQ(s->output_buffer.size(), 200u * 1024u);
    // Tail should still be z's.
    EXPECT_EQ(s->output_buffer.back(), 'z');
}

TEST_F(ProcessRegistryTest, WatchPatternFires) {
    ProcessRegistry reg(cp_);
    ProcessSession s = make_session("p1");
    s.watch_patterns = {"ERROR:"};
    reg.register_process(std::move(s));
    reg.feed_output("p1", "[INFO] starting up\n[INFO] loading config\n");
    reg.feed_output("p1", "[ERROR: database connection failed]\n");

    auto notes = reg.drain_notifications();
    ASSERT_EQ(notes.size(), 1u);
    EXPECT_EQ(notes[0].pattern, "ERROR:");
    EXPECT_NE(notes[0].line.find("database connection failed"),
              std::string::npos);
}

TEST_F(ProcessRegistryTest, RateLimitCapsAtEightPerTenSeconds) {
    ProcessRegistry reg(cp_);
    ProcessSession s = make_session("p1");
    s.watch_patterns = {"HIT"};
    reg.register_process(std::move(s));

    // Shovel 20 matching lines in a single chunk.
    std::string big;
    for (int i = 0; i < 20; ++i) {
        big += "HIT line " + std::to_string(i) + "\n";
    }
    reg.feed_output("p1", big);

    auto notes = reg.drain_notifications();
    EXPECT_EQ(notes.size(), 8u);
}

TEST_F(ProcessRegistryTest, SustainedOverloadKillsProcess) {
    ProcessRegistry reg(cp_);
    ProcessSession s = make_session("p1");
    s.watch_patterns = {"HIT"};
    // Fast-forward the overload clock by priming it manually.
    s.window_start = std::chrono::system_clock::now() - std::chrono::seconds(1);
    s.notifications_in_window = 8;  // already over the cap
    s.overload_start =
        std::chrono::system_clock::now() - std::chrono::seconds(50);
    reg.register_process(std::move(s));

    // One more matching line should trip the overload kill.
    reg.feed_output("p1", "HIT again\n");

    auto fetched = reg.get("p1");
    ASSERT_TRUE(fetched);
    EXPECT_EQ(fetched->state, ProcessState::Killed);

    auto notes = reg.drain_notifications();
    bool found_synthetic = false;
    for (const auto& n : notes) {
        if (n.synthetic) found_synthetic = true;
    }
    EXPECT_TRUE(found_synthetic);
}

TEST_F(ProcessRegistryTest, CheckpointAndRestore) {
    {
        ProcessRegistry reg(cp_);
        reg.register_process(make_session("p1"));
        ProcessSession s = make_session("p2");
        s.state = ProcessState::Running;
        reg.register_process(std::move(s));
        reg.mark_exited("p1", 0);
        reg.checkpoint();
    }
    EXPECT_TRUE(fs::exists(cp_));

    ProcessRegistry reg2(cp_);
    reg2.restore_from_checkpoint();
    // p1 exited → finished bucket; p2 was running → orphaned.
    auto orphans = reg2.orphaned();
    EXPECT_EQ(orphans.size(), 1u);
    EXPECT_EQ(orphans[0].id, "p2");

    auto finished = reg2.list_finished();
    ASSERT_EQ(finished.size(), 1u);
    EXPECT_EQ(finished[0].id, "p1");
}

TEST_F(ProcessRegistryTest, MarkExitedMovesToFinished) {
    ProcessRegistry reg(cp_);
    reg.register_process(make_session("p1"));
    reg.mark_exited("p1", 7);
    auto fetched = reg.get("p1");
    ASSERT_TRUE(fetched);
    EXPECT_EQ(fetched->state, ProcessState::Exited);
    ASSERT_TRUE(fetched->exit_code.has_value());
    EXPECT_EQ(*fetched->exit_code, 7);
    EXPECT_EQ(reg.list_running().size(), 0u);
    EXPECT_EQ(reg.list_finished().size(), 1u);
}
