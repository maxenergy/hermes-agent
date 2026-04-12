#include "hermes/environments/docker.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdlib>

namespace he = hermes::environments;

// Helper to check if a value exists in the args vector.
static bool has_arg(const std::vector<std::string>& args,
                    const std::string& value) {
    return std::find(args.begin(), args.end(), value) != args.end();
}

// Helper to find the value after a flag.
static std::string arg_after(const std::vector<std::string>& args,
                             const std::string& flag) {
    auto it = std::find(args.begin(), args.end(), flag);
    if (it != args.end() && ++it != args.end()) return *it;
    return {};
}

TEST(DockerEnvironment, BuildDockerArgsCapDrop) {
    he::DockerEnvironment::Config config;
    config.cap_drop_all = true;
    he::DockerEnvironment env(config);
    he::ExecuteOptions opts;

    auto args = env.build_docker_args(opts);
    EXPECT_TRUE(has_arg(args, "--cap-drop=ALL"));
}

TEST(DockerEnvironment, BuildDockerArgsNoNewPrivileges) {
    he::DockerEnvironment::Config config;
    config.no_new_privileges = true;
    he::DockerEnvironment env(config);
    he::ExecuteOptions opts;

    auto args = env.build_docker_args(opts);
    EXPECT_TRUE(has_arg(args, "--security-opt"));
    EXPECT_TRUE(has_arg(args, "no-new-privileges"));
}

TEST(DockerEnvironment, BuildDockerArgsPidsLimit) {
    he::DockerEnvironment::Config config;
    config.pids_limit = 100;
    he::DockerEnvironment env(config);
    he::ExecuteOptions opts;

    auto args = env.build_docker_args(opts);
    EXPECT_EQ(arg_after(args, "--pids-limit"), "100");
}

TEST(DockerEnvironment, BuildDockerArgsCpus) {
    he::DockerEnvironment::Config config;
    config.cpus = 2.5;
    he::DockerEnvironment env(config);
    he::ExecuteOptions opts;

    auto args = env.build_docker_args(opts);
    EXPECT_EQ(arg_after(args, "--cpus"), "2.5");
}

TEST(DockerEnvironment, BuildDockerArgsMemory) {
    he::DockerEnvironment::Config config;
    config.memory = "512m";
    he::DockerEnvironment env(config);
    he::ExecuteOptions opts;

    auto args = env.build_docker_args(opts);
    EXPECT_EQ(arg_after(args, "--memory"), "512m");
}

TEST(DockerEnvironment, BuildDockerArgsMounts) {
    he::DockerEnvironment::Config config;
    config.bind_mounts = {"/host/path:/container/path"};
    config.tmpfs_mounts = {"/tmp:size=64m"};
    he::DockerEnvironment env(config);
    he::ExecuteOptions opts;

    auto args = env.build_docker_args(opts);
    EXPECT_EQ(arg_after(args, "-v"), "/host/path:/container/path");
    EXPECT_EQ(arg_after(args, "--tmpfs"), "/tmp:size=64m");
}

TEST(DockerEnvironment, BuildDockerArgsEnvFiltered) {
    he::DockerEnvironment::Config config;
    he::DockerEnvironment env(config);
    he::ExecuteOptions opts;
    opts.env_vars = {
        {"NORMAL", "ok"},
        {"MY_API_KEY", "secret"},
    };

    auto args = env.build_docker_args(opts);
    // NORMAL should appear, MY_API_KEY should not.
    bool found_normal = false;
    bool found_secret = false;
    for (const auto& arg : args) {
        if (arg.find("NORMAL=ok") != std::string::npos) found_normal = true;
        if (arg.find("MY_API_KEY") != std::string::npos) found_secret = true;
    }
    EXPECT_TRUE(found_normal);
    EXPECT_FALSE(found_secret);
}

TEST(DockerEnvironment, BuildDockerArgsWorkingDir) {
    he::DockerEnvironment::Config config;
    he::DockerEnvironment env(config);
    he::ExecuteOptions opts;
    opts.cwd = "/workspace";

    auto args = env.build_docker_args(opts);
    EXPECT_EQ(arg_after(args, "-w"), "/workspace");
}

// E2E test gated on DOCKER_TEST=1.
TEST(DockerEnvironment, E2E_EchoHello) {
    const char* docker_test = std::getenv("DOCKER_TEST");
    if (!docker_test || std::string(docker_test) != "1") {
        GTEST_SKIP() << "Set DOCKER_TEST=1 to run Docker E2E tests";
    }

    he::DockerEnvironment::Config config;
    config.image = "ubuntu:24.04";
    he::DockerEnvironment env(config);
    he::ExecuteOptions opts;

    auto result = env.execute("echo hello", opts);
    EXPECT_EQ(result.exit_code, 0);
    EXPECT_NE(result.stdout_text.find("hello"), std::string::npos);
}
