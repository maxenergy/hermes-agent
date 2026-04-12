#include "hermes/environments/local.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <thread>

namespace he = hermes::environments;

#ifndef _WIN32

TEST(LocalEnvironment, EchoHello) {
    he::LocalEnvironment env;
    he::ExecuteOptions opts;
    auto result = env.execute("echo hello", opts);

    EXPECT_EQ(result.exit_code, 0);
    EXPECT_NE(result.stdout_text.find("hello"), std::string::npos);
}

TEST(LocalEnvironment, ExitCode42) {
    he::LocalEnvironment env;
    he::ExecuteOptions opts;
    auto result = env.execute("exit 42", opts);

    EXPECT_EQ(result.exit_code, 42);
}

TEST(LocalEnvironment, TimeoutTriggered) {
    he::LocalEnvironment env;
    he::ExecuteOptions opts;
    opts.timeout = std::chrono::seconds(1);
    auto result = env.execute("sleep 10", opts);

    EXPECT_TRUE(result.timed_out);
    EXPECT_GT(result.duration.count(), 0);
}

TEST(LocalEnvironment, CancelFn) {
    he::LocalEnvironment env;
    he::ExecuteOptions opts;
    opts.timeout = std::chrono::seconds(30);

    std::atomic<bool> cancel{false};
    opts.cancel_fn = [&cancel]() { return cancel.load(); };

    // Start a long-running command and cancel after a short delay.
    std::thread canceller([&cancel]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        cancel.store(true);
    });

    auto result = env.execute("sleep 30", opts);
    canceller.join();

    EXPECT_TRUE(result.interrupted);
}

TEST(LocalEnvironment, SensitiveEnvFiltered) {
    he::LocalEnvironment env;
    he::ExecuteOptions opts;
    opts.env_vars = {
        {"NORMAL_VAR", "visible"},
        {"MY_API_KEY", "should-be-filtered"},
    };

    // echo the env — the secret should NOT appear.
    auto result = env.execute("env", opts);
    EXPECT_EQ(result.exit_code, 0);
    EXPECT_NE(result.stdout_text.find("NORMAL_VAR"), std::string::npos);
    EXPECT_EQ(result.stdout_text.find("MY_API_KEY"), std::string::npos);
}

TEST(LocalEnvironment, CwdTracking) {
    he::LocalEnvironment env;
    he::ExecuteOptions opts;
    opts.cwd = "/tmp";

    auto result = env.execute("cd / && echo done", opts);
    EXPECT_EQ(result.exit_code, 0);
    // The final_cwd should be "/" since we cd'd there.
    EXPECT_EQ(result.final_cwd, std::filesystem::path("/"));
}

TEST(LocalEnvironment, StderrCaptured) {
    he::LocalEnvironment env;
    he::ExecuteOptions opts;
    auto result = env.execute("echo oops >&2", opts);

    EXPECT_EQ(result.exit_code, 0);
    EXPECT_NE(result.stderr_text.find("oops"), std::string::npos);
}

#endif  // _WIN32
