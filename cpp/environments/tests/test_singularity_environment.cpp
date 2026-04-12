#include "hermes/environments/singularity.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdlib>

namespace he = hermes::environments;

static bool has_arg(const std::vector<std::string>& args,
                    const std::string& value) {
    return std::find(args.begin(), args.end(), value) != args.end();
}

static std::string arg_after(const std::vector<std::string>& args,
                             const std::string& flag) {
    auto it = std::find(args.begin(), args.end(), flag);
    if (it != args.end() && ++it != args.end()) return *it;
    return {};
}

TEST(SingularityEnvironment, DetectBinaryReturnsSomething) {
    auto bin = he::SingularityEnvironment::detect_binary();
    EXPECT_TRUE(bin == "apptainer" || bin == "singularity");
}

TEST(SingularityEnvironment, BuildArgsContainallAndNoHome) {
    he::SingularityEnvironment::Config cfg;
    cfg.containall = true;
    cfg.no_home = true;
    cfg.binary = "apptainer";
    he::SingularityEnvironment env(cfg);

    he::ExecuteOptions opts;
    auto args = env.build_singularity_args("echo hi", opts);
    EXPECT_EQ(args.front(), "exec");
    EXPECT_TRUE(has_arg(args, "--containall"));
    EXPECT_TRUE(has_arg(args, "--no-home"));
    // bash -c "cmd" at the tail.
    ASSERT_GE(args.size(), 3u);
    EXPECT_EQ(args[args.size() - 3], "bash");
    EXPECT_EQ(args[args.size() - 2], "-c");
    EXPECT_EQ(args[args.size() - 1], "echo hi");
}

TEST(SingularityEnvironment, BuildArgsOverlayAndBind) {
    he::SingularityEnvironment::Config cfg;
    cfg.overlay_dir = "/var/lib/overlay";
    cfg.bind_mounts = {"/host:/container", "/host2:/container2"};
    cfg.binary = "apptainer";
    he::SingularityEnvironment env(cfg);

    he::ExecuteOptions opts;
    auto args = env.build_singularity_args("ls", opts);
    EXPECT_EQ(arg_after(args, "--overlay"), "/var/lib/overlay");
    EXPECT_TRUE(has_arg(args, "/host:/container"));
    EXPECT_TRUE(has_arg(args, "/host2:/container2"));

    // Two --bind flags total.
    int bind_count = 0;
    for (const auto& a : args) if (a == "--bind") ++bind_count;
    EXPECT_EQ(bind_count, 2);
}

TEST(SingularityEnvironment, BuildArgsDropCapabilities) {
    he::SingularityEnvironment::Config cfg;
    cfg.capabilities_drop = {"CAP_NET_RAW", "CAP_SYS_ADMIN"};
    cfg.binary = "apptainer";
    he::SingularityEnvironment env(cfg);

    he::ExecuteOptions opts;
    auto args = env.build_singularity_args("ls", opts);
    EXPECT_EQ(arg_after(args, "--drop-caps"), "CAP_NET_RAW,CAP_SYS_ADMIN");
}

TEST(SingularityEnvironment, BuildArgsPwdAndEnv) {
    he::SingularityEnvironment::Config cfg;
    cfg.binary = "apptainer";
    he::SingularityEnvironment env(cfg);

    he::ExecuteOptions opts;
    opts.cwd = "/work";
    opts.env_vars = {{"FOO", "bar"}, {"MY_API_KEY", "secret"}};

    auto args = env.build_singularity_args("ls", opts);
    EXPECT_EQ(arg_after(args, "--pwd"), "/work");

    // FOO should appear, MY_API_KEY should be filtered out.
    bool saw_foo = false;
    bool saw_key = false;
    for (const auto& a : args) {
        if (a == "FOO=bar") saw_foo = true;
        if (a.find("MY_API_KEY") != std::string::npos) saw_key = true;
    }
    EXPECT_TRUE(saw_foo);
    EXPECT_FALSE(saw_key);
}

TEST(SingularityEnvironment, ImageIsPositionalBeforeBash) {
    he::SingularityEnvironment::Config cfg;
    cfg.image = "docker://alpine:3.19";
    cfg.binary = "apptainer";
    he::SingularityEnvironment env(cfg);

    he::ExecuteOptions opts;
    auto args = env.build_singularity_args("true", opts);

    auto it = std::find(args.begin(), args.end(), "docker://alpine:3.19");
    ASSERT_NE(it, args.end());
    ASSERT_NE(std::next(it), args.end());
    EXPECT_EQ(*std::next(it), "bash");
}

// E2E test, gated on SINGULARITY_TEST=1.
TEST(SingularityEnvironment, E2E_EchoHello) {
    const char* gate = std::getenv("SINGULARITY_TEST");
    if (!gate || std::string(gate) != "1") {
        GTEST_SKIP() << "Set SINGULARITY_TEST=1 to run Singularity E2E tests";
    }

    he::SingularityEnvironment::Config cfg;
    cfg.image = "docker://alpine:3.19";
    he::SingularityEnvironment env(cfg);
    he::ExecuteOptions opts;
    auto result = env.execute("echo hello", opts);
    EXPECT_EQ(result.exit_code, 0);
    EXPECT_NE(result.stdout_text.find("hello"), std::string::npos);
}
