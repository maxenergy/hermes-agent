#include "hermes/cli/hermes_cli.hpp"

#include "hermes/tools/browser_backend.hpp"

#include <cstdlib>
#include <sstream>

#include <gtest/gtest.h>

using namespace hermes::cli;

TEST(HermesCLI, ProcessCommandHelp) {
    HermesCLI cli;
    EXPECT_TRUE(cli.process_command("/help"));
}

TEST(HermesCLI, ProcessCommandExit) {
    HermesCLI cli;
    EXPECT_TRUE(cli.process_command("/exit"));
}

TEST(HermesCLI, ProcessCommandUnknown) {
    HermesCLI cli;
    EXPECT_FALSE(cli.process_command("/xyzzy_nonexistent_command"));
}

TEST(HermesCLI, ShowBannerDoesNotCrash) {
    HermesCLI cli;
    EXPECT_NO_THROW(cli.show_banner());
}

TEST(HermesCLI, ProcessCommandNew) {
    HermesCLI cli;
    EXPECT_TRUE(cli.process_command("/new"));
}

TEST(HermesCLI, ProcessCommandModel) {
    HermesCLI cli;
    EXPECT_TRUE(cli.process_command("/model"));
    EXPECT_TRUE(cli.process_command("/model gpt-4o"));
}

TEST(HermesCLI, ProcessCommandUsage) {
    HermesCLI cli;
    EXPECT_TRUE(cli.process_command("/usage"));
}

TEST(HermesCLI, ProcessCommandStatus) {
    HermesCLI cli;
    EXPECT_TRUE(cli.process_command("/status"));
}

// ─── /browser ───────────────────────────────────────────────────────────
//
// These cover the state-machine surface without launching Chromium.  A
// separate live smoke (env HERMES_LIVE_BROWSER=1) exercises the real CDP
// path; here we only need `/browser status` and `/browser disconnect` to
// stay composable in the disconnected state.

namespace {
class CaptureStdout {
public:
    CaptureStdout() : prev_(std::cout.rdbuf(buf_.rdbuf())) {}
    ~CaptureStdout() { std::cout.rdbuf(prev_); }
    std::string str() const { return buf_.str(); }
private:
    std::stringstream buf_;
    std::streambuf* prev_;
};
}  // namespace

TEST(HermesCLI, BrowserHandleStatusWhenDisconnected) {
    HermesCLI cli;
    CaptureStdout cap;
    EXPECT_TRUE(cli.process_command("/browser status"));
    auto s = cap.str();
    EXPECT_NE(s.find("not connected"), std::string::npos)
        << "expected 'not connected' in status output, got: " << s;
}

TEST(HermesCLI, BrowserHandleStatusAliasEmpty) {
    // `/browser` with no args must default to status, not print a usage
    // error.  Regression guard: earlier the stub printed the "Usage:" line.
    HermesCLI cli;
    CaptureStdout cap;
    EXPECT_TRUE(cli.process_command("/browser"));
    auto s = cap.str();
    EXPECT_NE(s.find("not connected"), std::string::npos);
    EXPECT_EQ(s.find("Usage:"), std::string::npos);
}

TEST(HermesCLI, BrowserHandleDisconnectWhenDisconnected) {
    HermesCLI cli;
    CaptureStdout cap;
    EXPECT_TRUE(cli.process_command("/browser disconnect"));
    EXPECT_NE(cap.str().find("not connected"), std::string::npos);
}

TEST(HermesCLI, BrowserHandleUnknownSubcommand) {
    HermesCLI cli;
    CaptureStdout cap;
    EXPECT_TRUE(cli.process_command("/browser bogus"));
    EXPECT_NE(cap.str().find("Usage:"), std::string::npos);
}

TEST(HermesCLI, BrowserConnectWithFakeBackend) {
    // HERMES_BROWSER_FAKE=1 short-circuits the real Chromium spawn so we
    // can still verify that `/browser connect` flips state and that the
    // global BrowserBackend is installed (tool layer will now find it).
    ::setenv("HERMES_BROWSER_FAKE", "1", 1);
    // Drop any stray backend left by a previous test.
    hermes::tools::set_browser_backend(nullptr);
    {
        HermesCLI cli;
        {
            CaptureStdout cap;
            EXPECT_TRUE(cli.process_command("/browser connect"));
            EXPECT_NE(cap.str().find("fake backend"), std::string::npos);
        }
        EXPECT_NE(hermes::tools::get_browser_backend(), nullptr);
        {
            CaptureStdout cap;
            EXPECT_TRUE(cli.process_command("/browser status"));
            // With HERMES_BROWSER_FAKE there's no CdpBackend, but
            // browser_connected_ is true so status should not say "not".
            auto s = cap.str();
            EXPECT_NE(s.find("connected"), std::string::npos);
            EXPECT_EQ(s.find("not connected"), std::string::npos);
        }
        {
            CaptureStdout cap;
            EXPECT_TRUE(cli.process_command("/browser disconnect"));
            EXPECT_NE(cap.str().find("disconnected"), std::string::npos);
        }
        EXPECT_EQ(hermes::tools::get_browser_backend(), nullptr);
    }
    // HermesCLI destructor must have left the global cleared either way.
    EXPECT_EQ(hermes::tools::get_browser_backend(), nullptr);
    ::unsetenv("HERMES_BROWSER_FAKE");
}

#ifdef HERMES_ENABLE_LIVE_BROWSER
// Live test — launches a real Chromium process.  Disabled by default
// because CI runners rarely have a browser binary; enable via
//   cmake -DHERMES_ENABLE_LIVE_BROWSER=ON ...
TEST(HermesCLI, BrowserConnectLive) {
    HermesCLI cli;
    EXPECT_TRUE(cli.process_command("/browser connect"));
    EXPECT_NE(hermes::tools::get_browser_backend(), nullptr);
    EXPECT_TRUE(cli.process_command("/browser disconnect"));
}
#endif
