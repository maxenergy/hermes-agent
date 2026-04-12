#include "hermes/environments/local.hpp"

#include <gtest/gtest.h>

namespace he = hermes::environments;

// PTY tests are Linux/macOS only.
#if defined(__linux__) || defined(__APPLE__)

TEST(LocalEnvironment, PtyEchoTest) {
    he::LocalEnvironment env;
    he::ExecuteOptions opts;
    opts.use_pty = true;
    auto result = env.execute("echo pty-test", opts);

    EXPECT_EQ(result.exit_code, 0);
    EXPECT_NE(result.stdout_text.find("pty-test"), std::string::npos);
}

TEST(LocalEnvironment, PtyExitCode) {
    he::LocalEnvironment env;
    he::ExecuteOptions opts;
    opts.use_pty = true;
    auto result = env.execute("exit 42", opts);

    EXPECT_EQ(result.exit_code, 42);
}

TEST(LocalEnvironment, PtyTimeoutTriggered) {
    he::LocalEnvironment env;
    he::ExecuteOptions opts;
    opts.use_pty = true;
    opts.timeout = std::chrono::seconds(1);
    auto result = env.execute("sleep 10", opts);

    EXPECT_TRUE(result.timed_out);
}

TEST(LocalEnvironment, PtyStderrMerged) {
    // In PTY mode, stderr is merged into stdout.
    he::LocalEnvironment env;
    he::ExecuteOptions opts;
    opts.use_pty = true;
    auto result = env.execute("echo oops >&2", opts);

    EXPECT_EQ(result.exit_code, 0);
    // stderr goes to the PTY's stdout side.
    EXPECT_NE(result.stdout_text.find("oops"), std::string::npos);
}

#else

TEST(LocalEnvironment, PtyFallbackToPipe) {
    // On systems without PTY, use_pty=true should fall back to pipe.
    he::LocalEnvironment env;
    he::ExecuteOptions opts;
    opts.use_pty = true;
    auto result = env.execute("echo fallback-test", opts);

    EXPECT_EQ(result.exit_code, 0);
    EXPECT_NE(result.stdout_text.find("fallback-test"), std::string::npos);
}

#endif
