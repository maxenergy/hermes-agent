#include "hermes/environments/scp_copier.hpp"

#include <gtest/gtest.h>

#include <algorithm>

namespace he = hermes::environments;

static bool has(const std::vector<std::string>& v, const std::string& s) {
    return std::find(v.begin(), v.end(), s) != v.end();
}

TEST(ScpCopier, BuildArgvHasBatchAndPreserveFlags) {
    he::ScpCopier::Config cfg;
    cfg.target = "user@host";
    he::ScpCopier c(cfg);
    auto argv = c.build_argv("/tmp/foo", "/remote/foo");
    EXPECT_EQ(argv.front(), "scp");
    EXPECT_TRUE(has(argv, "-p"));
    EXPECT_TRUE(has(argv, "-B"));
    EXPECT_TRUE(has(argv, "-q"));
    EXPECT_EQ(argv.back(), "user@host:/remote/foo");
    EXPECT_TRUE(has(argv, "/tmp/foo"));
}

TEST(ScpCopier, BuildArgvAddsPortWhenSpecified) {
    he::ScpCopier::Config cfg;
    cfg.target = "box";
    cfg.port = 2222;
    he::ScpCopier c(cfg);
    auto argv = c.build_argv("a", "b");
    auto it = std::find(argv.begin(), argv.end(), "-P");
    ASSERT_NE(it, argv.end());
    EXPECT_EQ(*(it + 1), "2222");
}

TEST(ScpCopier, BuildArgvInjectsSshOptions) {
    he::ScpCopier::Config cfg;
    cfg.target = "box";
    cfg.ssh_options = {"StrictHostKeyChecking=no",
                       "UserKnownHostsFile=/dev/null"};
    he::ScpCopier c(cfg);
    auto argv = c.build_argv("a", "b");
    EXPECT_TRUE(has(argv, "StrictHostKeyChecking=no"));
    EXPECT_TRUE(has(argv, "UserKnownHostsFile=/dev/null"));
}

TEST(ScpCopier, BuildArgvAddsControlPath) {
    he::ScpCopier::Config cfg;
    cfg.target = "box";
    cfg.control_path = "/tmp/cm-sock";
    he::ScpCopier c(cfg);
    auto argv = c.build_argv("a", "b");
    EXPECT_TRUE(has(argv, "ControlPath=/tmp/cm-sock"));
}

TEST(ScpCopier, RunnerCalledWithBuiltArgv) {
    he::ScpCopier::Config cfg;
    cfg.target = "alice@host";
    he::ScpCopier c(cfg);
    std::vector<std::string> seen;
    c.set_runner([&](const std::vector<std::string>& v) {
        seen = v;
        return 0;
    });
    EXPECT_TRUE(c.copy("/src", "/dst"));
    EXPECT_EQ(seen.front(), "scp");
    EXPECT_EQ(seen.back(), "alice@host:/dst");
}

TEST(ScpCopier, NonZeroExitCausesFailure) {
    he::ScpCopier c;
    c.set_runner([&](const std::vector<std::string>&) { return 7; });
    EXPECT_FALSE(c.copy("a", "b"));
}

TEST(ScpCopier, AsCopyFnBindsConfig) {
    he::ScpCopier::Config cfg;
    cfg.target = "t";
    he::ScpCopier c(cfg);
    std::string last_target;
    c.set_runner([&](const std::vector<std::string>& v) {
        last_target = v.back();
        return 0;
    });
    auto fn = c.as_copy_fn();
    EXPECT_TRUE(fn("/local", "/remote/path"));
    EXPECT_EQ(last_target, "t:/remote/path");
}
