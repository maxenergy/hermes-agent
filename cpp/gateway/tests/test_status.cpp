#include <gtest/gtest.h>

#include <hermes/gateway/status.hpp>

#include <filesystem>

namespace hg = hermes::gateway;
namespace fs = std::filesystem;

class StatusTest : public ::testing::Test {
protected:
    fs::path original_home_;
    fs::path tmp_dir_;

    void SetUp() override {
        tmp_dir_ = fs::temp_directory_path() /
                   ("hermes_status_test_" +
                    std::to_string(std::chrono::system_clock::now()
                                       .time_since_epoch()
                                       .count()));
        fs::create_directories(tmp_dir_);

        // Override HERMES_HOME so tests don't write to real home.
        original_home_ = std::getenv("HERMES_HOME")
                             ? std::getenv("HERMES_HOME")
                             : "";
        setenv("HERMES_HOME", tmp_dir_.c_str(), 1);

        // Override lock dir too.
        setenv("HERMES_GATEWAY_LOCK_DIR",
               (tmp_dir_ / "locks").c_str(), 1);
    }

    void TearDown() override {
        if (original_home_.empty()) {
            unsetenv("HERMES_HOME");
        } else {
            setenv("HERMES_HOME", original_home_.c_str(), 1);
        }
        unsetenv("HERMES_GATEWAY_LOCK_DIR");
        fs::remove_all(tmp_dir_);
    }
};

TEST_F(StatusTest, PidFileWriteAndRead) {
    hg::write_pid_file();
    auto pid = hg::read_pid_file();
    ASSERT_TRUE(pid.has_value());
    EXPECT_EQ(*pid, static_cast<int>(getpid()));
}

TEST_F(StatusTest, ScopedLockAcquireAndRelease) {
    bool acquired =
        hg::acquire_scoped_lock("telegram", "bot_token_abc");
    EXPECT_TRUE(acquired);

    // Same lock again from same process — should still succeed since
    // process is the same PID.  (In real usage, a second process would
    // fail.)

    hg::release_scoped_lock("telegram", "bot_token_abc");

    // After release, acquire should succeed.
    bool acquired2 =
        hg::acquire_scoped_lock("telegram", "bot_token_abc");
    EXPECT_TRUE(acquired2);
    hg::release_scoped_lock("telegram", "bot_token_abc");
}

TEST_F(StatusTest, RuntimeStatusRoundTrip) {
    hg::RuntimeStatus status;
    status.state = "running";
    status.exit_reason = "";
    status.restart_requested = true;
    status.platform_states[hg::Platform::Telegram] = "connected";
    status.platform_states[hg::Platform::Discord] = "disconnected";
    status.timestamp = std::chrono::system_clock::now();

    hg::write_runtime_status(status);
    auto loaded = hg::read_runtime_status();

    ASSERT_TRUE(loaded.has_value());
    EXPECT_EQ(loaded->state, "running");
    EXPECT_TRUE(loaded->restart_requested);
    EXPECT_EQ(loaded->platform_states.at(hg::Platform::Telegram),
              "connected");
    EXPECT_EQ(loaded->platform_states.at(hg::Platform::Discord),
              "disconnected");
}

TEST_F(StatusTest, IsGatewayRunningWithCurrentProcess) {
    hg::write_pid_file();
    EXPECT_TRUE(hg::is_gateway_running());
}
