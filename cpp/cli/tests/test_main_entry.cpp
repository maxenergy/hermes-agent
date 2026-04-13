#include "hermes/cli/main_entry.hpp"

#include <gtest/gtest.h>

#include <sstream>
#include <string>

using namespace hermes::cli;

// Helper: capture stdout during a callable.
namespace {
template <typename Fn>
std::string capture_stdout(Fn fn) {
    std::ostringstream oss;
    auto* old = std::cout.rdbuf(oss.rdbuf());
    fn();
    std::cout.rdbuf(old);
    return oss.str();
}
}  // namespace

TEST(MainEntry, VersionPrintsString) {
    auto out = capture_stdout([] { cmd_version(); });
    EXPECT_NE(out.find("hermes-cpp"), std::string::npos);
    EXPECT_NE(out.find("0.1.0"), std::string::npos);
}

TEST(MainEntry, DoctorDoesNotCrash) {
    // Doctor may fail some checks (e.g. no config file in test env) but
    // must not crash or throw.
    EXPECT_NO_THROW({
        std::ostringstream oss;
        auto* old = std::cout.rdbuf(oss.rdbuf());
        cmd_doctor();
        std::cout.rdbuf(old);
    });
}

TEST(MainEntry, StatusDoesNotCrash) {
    EXPECT_NO_THROW({
        std::ostringstream oss;
        auto* old = std::cout.rdbuf(oss.rdbuf());
        cmd_status();
        std::cout.rdbuf(old);
    });
}

TEST(MainEntry, ConfigSetUpdatesValue) {
    // hermes config set test_key test_value
    // We can't easily test persistent save in unit tests, but we can
    // verify cmd_config doesn't crash with proper argc/argv.
    const char* argv[] = {"hermes", "config", "set", "test_key_xyz", "42"};
    EXPECT_NO_THROW({
        std::ostringstream oss;
        auto* old_out = std::cout.rdbuf(oss.rdbuf());
        auto* old_err = std::cerr.rdbuf(oss.rdbuf());
        // May fail due to filesystem but should not crash.
        cmd_config(5, const_cast<char**>(argv));
        std::cout.rdbuf(old_out);
        std::cerr.rdbuf(old_err);
    });
}

TEST(MainEntry, UnknownSubcommandPrintsHelp) {
    const char* argv[] = {"hermes", "xyzzy_bogus"};
    auto out = capture_stdout([&] {
        std::ostringstream oss_err;
        auto* old_err = std::cerr.rdbuf(oss_err.rdbuf());
        main_entry(2, const_cast<char**>(argv));
        std::cerr.rdbuf(old_err);
    });
    // The help text is printed to cout.
    EXPECT_NE(out.find("Subcommands"), std::string::npos);
}

TEST(MainEntry, ToolsDoesNotCrash) {
    EXPECT_NO_THROW({
        std::ostringstream oss;
        auto* old = std::cout.rdbuf(oss.rdbuf());
        cmd_tools();
        std::cout.rdbuf(old);
    });
}

TEST(MainEntry, GatewayStartStub) {
    const char* argv[] = {"hermes", "gateway", "start"};
    auto out = capture_stdout([&] {
        cmd_gateway(3, const_cast<char**>(argv));
    });
    EXPECT_NE(out.find("Starting gateway"), std::string::npos);
}

TEST(MainEntry, GatewayStopStub) {
    const char* argv[] = {"hermes", "gateway", "stop"};
    auto out = capture_stdout([&] {
        cmd_gateway(3, const_cast<char**>(argv));
    });
    EXPECT_NE(out.find("Stopping gateway"), std::string::npos);
}

TEST(MainEntry, VersionStringConstant) {
    std::string v = kVersionString;
    EXPECT_NE(v.find("hermes-cpp"), std::string::npos);
}
