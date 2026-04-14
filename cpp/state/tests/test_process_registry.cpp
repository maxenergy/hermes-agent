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

// ---------------------------------------------------------------------------
// spawn_local() tests — real fork/exec integration.
// These tests run actual /bin/true, /bin/false, /bin/echo, /bin/sleep so they
// require a POSIX environment.  Skipped automatically on Windows builds.
// ---------------------------------------------------------------------------
#ifndef _WIN32

namespace {
// Poll until `pred()` returns true or `timeout` elapses.  Returns the
// result of the last call to `pred()`.
template <typename Pred>
bool wait_until(Pred pred, std::chrono::milliseconds timeout,
                std::chrono::milliseconds step =
                    std::chrono::milliseconds(20)) {
    auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred()) return true;
        std::this_thread::sleep_for(step);
    }
    return pred();
}
}  // namespace

TEST_F(ProcessRegistryTest, SpawnLocalTrueExitsZero) {
    ProcessRegistry reg(cp_);
    hermes::state::SpawnOptions opts;
    opts.command = "/bin/true";
    auto id = reg.spawn_local(opts);
    ASSERT_FALSE(id.empty());

    ASSERT_TRUE(wait_until(
        [&]() {
            auto s = reg.get(id);
            return s && s->state == hermes::state::ProcessState::Exited;
        },
        std::chrono::milliseconds(3000)));

    auto s = reg.get(id);
    ASSERT_TRUE(s);
    ASSERT_TRUE(s->exit_code.has_value());
    EXPECT_EQ(*s->exit_code, 0);
}

TEST_F(ProcessRegistryTest, SpawnLocalFalseExitsOne) {
    ProcessRegistry reg(cp_);
    hermes::state::SpawnOptions opts;
    opts.command = "/bin/false";
    auto id = reg.spawn_local(opts);

    ASSERT_TRUE(wait_until(
        [&]() {
            auto s = reg.get(id);
            return s && s->state == hermes::state::ProcessState::Exited;
        },
        std::chrono::milliseconds(3000)));

    auto s = reg.get(id);
    ASSERT_TRUE(s);
    ASSERT_TRUE(s->exit_code.has_value());
    EXPECT_EQ(*s->exit_code, 1);
}

TEST_F(ProcessRegistryTest, SpawnLocalEchoCapturesStdout) {
    ProcessRegistry reg(cp_);
    hermes::state::SpawnOptions opts;
    opts.command = "/bin/echo hello-spawn-local";
    auto id = reg.spawn_local(opts);

    ASSERT_TRUE(wait_until(
        [&]() {
            auto s = reg.get(id);
            return s && s->state == hermes::state::ProcessState::Exited;
        },
        std::chrono::milliseconds(3000)));

    auto s = reg.get(id);
    ASSERT_TRUE(s);
    EXPECT_EQ(*s->exit_code, 0);
    EXPECT_NE(s->output_buffer.find("hello-spawn-local"), std::string::npos);
}

TEST_F(ProcessRegistryTest, SpawnLocalSleepTimesOut) {
    ProcessRegistry reg(cp_);
    hermes::state::SpawnOptions opts;
    opts.command = "/bin/sleep 5";
    opts.timeout = std::chrono::seconds(1);
    auto start = std::chrono::steady_clock::now();
    auto id = reg.spawn_local(opts);

    ASSERT_TRUE(wait_until(
        [&]() {
            auto s = reg.get(id);
            return s && s->state == hermes::state::ProcessState::Exited;
        },
        std::chrono::milliseconds(5000)));
    auto elapsed = std::chrono::steady_clock::now() - start;

    auto s = reg.get(id);
    ASSERT_TRUE(s);
    ASSERT_TRUE(s->exit_code.has_value());
    EXPECT_EQ(*s->exit_code, 124);  // timeout sentinel
    EXPECT_LT(std::chrono::duration_cast<std::chrono::seconds>(elapsed)
                  .count(),
              5);  // killed well before sleep would have finished
}

TEST_F(ProcessRegistryTest, SpawnLocalEnvVarsPassThrough) {
    ProcessRegistry reg(cp_);
    hermes::state::SpawnOptions opts;
    opts.command = "/bin/sh -c 'printf %s \"$HERMES_SPAWN_TEST\"'";
    opts.env_vars = {{"HERMES_SPAWN_TEST", "marker-value-12345"}};
    auto id = reg.spawn_local(opts);

    ASSERT_TRUE(wait_until(
        [&]() {
            auto s = reg.get(id);
            return s && s->state == hermes::state::ProcessState::Exited;
        },
        std::chrono::milliseconds(3000)));

    auto s = reg.get(id);
    ASSERT_TRUE(s);
    EXPECT_EQ(*s->exit_code, 0);
    EXPECT_NE(s->output_buffer.find("marker-value-12345"),
              std::string::npos);
}

TEST_F(ProcessRegistryTest, SpawnLocalCwdIsHonored) {
    ProcessRegistry reg(cp_);
    hermes::state::SpawnOptions opts;
    opts.command = "/bin/sh -c pwd";
    opts.cwd = dir_;  // the test's scratch directory
    auto id = reg.spawn_local(opts);

    ASSERT_TRUE(wait_until(
        [&]() {
            auto s = reg.get(id);
            return s && s->state == hermes::state::ProcessState::Exited;
        },
        std::chrono::milliseconds(3000)));

    auto s = reg.get(id);
    ASSERT_TRUE(s);
    EXPECT_EQ(*s->exit_code, 0);
    // pwd may resolve symlinks differently on macOS (/private/tmp vs /tmp);
    // check for the last path component which stays stable.
    auto leaf = dir_.filename().string();
    EXPECT_NE(s->output_buffer.find(leaf), std::string::npos);
}

TEST_F(ProcessRegistryTest, SpawnLocalKillViaRegistry) {
    ProcessRegistry reg(cp_);
    hermes::state::SpawnOptions opts;
    opts.command = "/bin/sleep 30";
    auto id = reg.spawn_local(opts);

    // Give the child a moment to actually be running.
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    auto before = reg.get(id);
    ASSERT_TRUE(before);
    ASSERT_EQ(before->state, hermes::state::ProcessState::Running);

    reg.kill(id);

    // kill() itself does the SIGTERM + 2s wait + SIGKILL, so by the time
    // it returns the process is gone.  But our waiter thread may still
    // be mid-waitpid — that's fine, the session is already in finished.
    auto s = reg.get(id);
    ASSERT_TRUE(s);
    EXPECT_EQ(s->state, hermes::state::ProcessState::Killed);
}

TEST_F(ProcessRegistryTest, SpawnLocalWatchPatternFires) {
    ProcessRegistry reg(cp_);
    hermes::state::SpawnOptions opts;
    opts.command = "/bin/sh -c 'echo info; echo ERROR: boom; echo done'";
    opts.watch_patterns = {"ERROR:"};
    auto id = reg.spawn_local(opts);

    ASSERT_TRUE(wait_until(
        [&]() {
            auto s = reg.get(id);
            return s && s->state == hermes::state::ProcessState::Exited;
        },
        std::chrono::milliseconds(3000)));

    auto notes = reg.drain_notifications();
    bool hit = false;
    for (const auto& n : notes) {
        if (n.pattern == "ERROR:" &&
            n.line.find("boom") != std::string::npos) {
            hit = true;
        }
    }
    EXPECT_TRUE(hit);
}

#endif  // !_WIN32

// =========================================================================
// Extended tests — stream-aware tails, persist sink, restart policy,
// resource limits (structural), and output buffer bookkeeping.
// =========================================================================

using hermes::state::RestartPolicy;
using hermes::state::ResourceLimits;
using hermes::state::SpawnOptions;

TEST_F(ProcessRegistryTest, AppendStreamRoutesCorrectly) {
    ProcessSession s = make_session("ps");
    s.stream_buffer_max = 1024;
    s.append_stream("hello ", 1);
    s.append_stream("err!\n", 2);
    s.append_stream("world\n", 1);
    EXPECT_EQ(s.tail(1, 1024), "hello world\n");
    EXPECT_EQ(s.tail(2, 1024), "err!\n");
    EXPECT_NE(s.tail(0, 1024).find("hello"), std::string::npos);
    EXPECT_NE(s.tail(0, 1024).find("err!"), std::string::npos);
}

TEST_F(ProcessRegistryTest, AppendStreamRollsIndependentCaps) {
    ProcessSession s = make_session("ps");
    s.stream_buffer_max = 8;
    s.append_stream("0123456789", 1);      // overflows stdout cap
    s.append_stream("ABCDE", 2);
    EXPECT_EQ(s.stdout_buffer.size(), 8u);
    EXPECT_EQ(s.stdout_buffer, "23456789");
    EXPECT_EQ(s.stderr_buffer, "ABCDE");
}

TEST_F(ProcessRegistryTest, TailReturnsLastN) {
    ProcessSession s = make_session("ps");
    s.stream_buffer_max = 1024;
    s.append_stream("abcdefghij", 1);
    EXPECT_EQ(s.tail(1, 3), "hij");
    EXPECT_EQ(s.tail(1, 100), "abcdefghij");
}

TEST_F(ProcessRegistryTest, RegistryTailStreamAware) {
    ProcessRegistry reg(cp_);
    auto session = make_session("ps");
    session.stream_buffer_max = 1024;
    auto id = reg.register_process(std::move(session));
    reg.feed_output_stream(id, "one\n", 1);
    reg.feed_output_stream(id, "two\n", 2);
    EXPECT_EQ(reg.tail(id, 1, 1024), "one\n");
    EXPECT_EQ(reg.tail(id, 2, 1024), "two\n");
    EXPECT_NE(reg.tail(id, 0, 1024).find("one\n"), std::string::npos);
    EXPECT_NE(reg.tail(id, 0, 1024).find("two\n"), std::string::npos);
}

TEST_F(ProcessRegistryTest, PersistTailOnlyFlushesNewBytes) {
    ProcessRegistry reg(cp_);
    auto id = reg.register_process(make_session("ps"));
    reg.feed_output(id, "hello ");
    std::string buf;
    auto n1 = reg.persist_tail(id,
        [&](std::string_view s) { buf.append(s); });
    EXPECT_EQ(n1, 6u);
    EXPECT_EQ(buf, "hello ");

    // Second call with no new output -> zero bytes.
    auto n2 = reg.persist_tail(id,
        [&](std::string_view s) { buf.append(s); });
    EXPECT_EQ(n2, 0u);

    // New input -> only the delta is flushed.
    reg.feed_output(id, "world\n");
    auto n3 = reg.persist_tail(id,
        [&](std::string_view s) { buf.append(s); });
    EXPECT_EQ(n3, 6u);
    EXPECT_EQ(buf, "hello world\n");
}

TEST_F(ProcessRegistryTest, PersistTailUnknownProcess) {
    ProcessRegistry reg(cp_);
    auto n = reg.persist_tail("does-not-exist",
                              [](std::string_view) {});
    EXPECT_EQ(n, 0u);
}

TEST_F(ProcessRegistryTest, PersistSinkSwallowsExceptions) {
    ProcessRegistry reg(cp_);
    auto id = reg.register_process(make_session("ps"));
    reg.feed_output(id, "data");
    // The sink throws, but the registry must not crash and must mark
    // the bytes persisted so a later retry doesn't duplicate them.
    (void)reg.persist_tail(id, [](std::string_view) {
        throw std::runtime_error("boom");
    });
    // Second call with the same content should still return 0 new
    // bytes (watermark advanced even though sink threw).
    std::string buf;
    auto n = reg.persist_tail(id,
        [&](std::string_view s) { buf.append(s); });
    EXPECT_EQ(n, 0u);
    EXPECT_TRUE(buf.empty());
}

TEST_F(ProcessRegistryTest, RestartPolicyRoundtrip) {
    ProcessRegistry reg(cp_);
    auto id = reg.register_process(make_session("ps"));
    RestartPolicy p;
    p.max_restarts = 3;
    p.backoff = std::chrono::milliseconds(250);
    p.restart_on_exit_codes = {137};
    p.restart_on_timeout = true;
    reg.set_restart_policy(id, p);
    auto got = reg.restart_policy(id);
    EXPECT_EQ(got.max_restarts, 3);
    EXPECT_EQ(got.backoff.count(), 250);
    ASSERT_EQ(got.restart_on_exit_codes.size(), 1u);
    EXPECT_EQ(got.restart_on_exit_codes[0], 137);
    EXPECT_TRUE(got.restart_on_timeout);
}

TEST_F(ProcessRegistryTest, MaybeRestartHonoursMaxAttempts) {
    ProcessRegistry reg(cp_);
    auto id = reg.register_process(make_session("ps"));
    RestartPolicy p;
    p.max_restarts = 2;
    reg.set_restart_policy(id, p);

    // First failure (exit_code=1) — allowed.
    reg.mark_exited(id, 1);
    EXPECT_TRUE(reg.maybe_restart(id, 1));
    // Second failure — allowed.
    reg.mark_exited(id, 1);
    EXPECT_TRUE(reg.maybe_restart(id, 1));
    // Third failure — blocked.
    reg.mark_exited(id, 1);
    EXPECT_FALSE(reg.maybe_restart(id, 1));
}

TEST_F(ProcessRegistryTest, MaybeRestartSkipsSuccessfulExits) {
    ProcessRegistry reg(cp_);
    auto id = reg.register_process(make_session("ps"));
    RestartPolicy p;
    p.max_restarts = 5;
    reg.set_restart_policy(id, p);
    reg.mark_exited(id, 0);
    EXPECT_FALSE(reg.maybe_restart(id, 0));
}

TEST_F(ProcessRegistryTest, MaybeRestartExitCodeFilter) {
    ProcessRegistry reg(cp_);
    auto id = reg.register_process(make_session("ps"));
    RestartPolicy p;
    p.max_restarts = 3;
    p.restart_on_exit_codes = {42};
    reg.set_restart_policy(id, p);

    reg.mark_exited(id, 99);
    EXPECT_FALSE(reg.maybe_restart(id, 99));

    // Re-register (it's been moved to finished).
    reg.register_process(make_session("ps"));
    reg.mark_exited(id, 42);
    EXPECT_TRUE(reg.maybe_restart(id, 42));
}

TEST_F(ProcessRegistryTest, MaybeRestartTimeoutGate) {
    ProcessRegistry reg(cp_);
    auto id = reg.register_process(make_session("ps"));

    RestartPolicy off;
    off.max_restarts = 2;
    off.restart_on_timeout = false;
    reg.set_restart_policy(id, off);
    reg.mark_exited(id, 124);
    EXPECT_FALSE(reg.maybe_restart(id, 124));

    // Re-register, enable timeout restart.
    reg.register_process(make_session("ps"));
    RestartPolicy on;
    on.max_restarts = 2;
    on.restart_on_timeout = true;
    reg.set_restart_policy(id, on);
    reg.mark_exited(id, 124);
    EXPECT_TRUE(reg.maybe_restart(id, 124));
}

TEST_F(ProcessRegistryTest, MaybeRestartUnknownId) {
    ProcessRegistry reg(cp_);
    EXPECT_FALSE(reg.maybe_restart("ghost", 1));
}

TEST_F(ProcessRegistryTest, RestartPolicyStructCopyable) {
    RestartPolicy p{};
    p.max_restarts = 5;
    p.restart_on_exit_codes = {1, 2, 3};
    RestartPolicy q = p;
    EXPECT_EQ(q.max_restarts, 5);
    EXPECT_EQ(q.restart_on_exit_codes.size(), 3u);
}

TEST_F(ProcessRegistryTest, ResourceLimitsStructDefaultsZero) {
    ResourceLimits l;
    EXPECT_EQ(l.cpu_seconds, 0u);
    EXPECT_EQ(l.memory_bytes, 0u);
    EXPECT_EQ(l.max_open_files, 0u);
    EXPECT_FALSE(l.disable_core_dumps);
}

TEST_F(ProcessRegistryTest, SpawnOptionsAcceptsLimitsAndRestart) {
    // Structural — confirm the extended fields compile and can be
    // populated.  We don't actually spawn here because that would
    // depend on the environment.
    SpawnOptions opts;
    opts.command = "echo";
    opts.limits.cpu_seconds = 10;
    opts.limits.memory_bytes = 256 * 1024 * 1024;
    opts.limits.disable_core_dumps = true;
    opts.restart.max_restarts = 2;
    opts.restart.restart_on_timeout = true;
    opts.persist_sink = [](const std::string&, int,
                            std::string_view) {};
    EXPECT_EQ(opts.limits.cpu_seconds, 10u);
    EXPECT_EQ(opts.restart.max_restarts, 2);
    EXPECT_TRUE(static_cast<bool>(opts.persist_sink));
}

#ifndef _WIN32
TEST_F(ProcessRegistryTest, SpawnLocalWithPersistSinkCapturesOutput) {
    ProcessRegistry reg(cp_);
    std::mutex m;
    std::string captured_stdout;
    std::string captured_stderr;
    SpawnOptions opts;
    opts.command = "printf 'out\\n'; printf 'err\\n' 1>&2";
    opts.persist_sink =
        [&](const std::string&, int stream, std::string_view chunk) {
            std::lock_guard<std::mutex> lk(m);
            if (stream == 1) captured_stdout.append(chunk);
            else captured_stderr.append(chunk);
        };
    auto id = reg.spawn_local(opts);
    // Wait up to 5s for the child to finish.
    for (int i = 0; i < 100; ++i) {
        auto s = reg.get(id);
        if (s && s->state != ProcessState::Running) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    std::lock_guard<std::mutex> lk(m);
    EXPECT_NE(captured_stdout.find("out"), std::string::npos);
    EXPECT_NE(captured_stderr.find("err"), std::string::npos);
    // Stream-aware tails should also see the split.
    EXPECT_NE(reg.tail(id, 1, 4096).find("out"), std::string::npos);
    EXPECT_NE(reg.tail(id, 2, 4096).find("err"), std::string::npos);
}
#endif  // !_WIN32

