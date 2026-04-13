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

// ---------------------------------------------------------------------------
// resolve_image_digest / pin_configured_image — using mocked runner.
// ---------------------------------------------------------------------------

TEST(DockerEnvironment, ResolveDigestReturnsInputWhenAlreadyPinned) {
    he::DockerEnvironment::Config config;
    config.image = "ubuntu@sha256:abcd1234";
    he::DockerEnvironment env(config);

    // Runner must NOT be called when image is already digest-pinned.
    bool runner_called = false;
    env.set_docker_runner([&](const std::vector<std::string>&) {
        runner_called = true;
        return he::CompletedProcess{};
    });

    auto out = env.resolve_image_digest("ubuntu@sha256:abcd1234");
    EXPECT_EQ(out, "ubuntu@sha256:abcd1234");
    EXPECT_FALSE(runner_called);
}

TEST(DockerEnvironment, ResolveDigestParsesVerboseManifest) {
    he::DockerEnvironment env;
    env.set_docker_runner([&](const std::vector<std::string>& argv) {
        he::CompletedProcess cp;
        cp.exit_code = 0;
        EXPECT_EQ(argv[0], "manifest");
        EXPECT_EQ(argv[1], "inspect");
        cp.stdout_text = R"({
            "Descriptor": {"digest": "sha256:deadbeef0000000000000000000000000000000000000000000000000000dead"},
            "SchemaV2Manifest": {}
        })";
        return cp;
    });

    auto pinned = env.resolve_image_digest("ubuntu:24.04");
    EXPECT_EQ(pinned,
              "ubuntu@sha256:deadbeef0000000000000000000000000000000000000000000000000000dead");
}

TEST(DockerEnvironment, ResolveDigestCachesResult) {
    he::DockerEnvironment env;
    int call_count = 0;
    env.set_docker_runner([&](const std::vector<std::string>&) {
        ++call_count;
        he::CompletedProcess cp;
        cp.exit_code = 0;
        cp.stdout_text = R"({"digest":"sha256:feedface"})";
        return cp;
    });

    (void)env.resolve_image_digest("ubuntu:22.04");
    (void)env.resolve_image_digest("ubuntu:22.04");
    (void)env.resolve_image_digest("ubuntu:22.04");
    EXPECT_EQ(call_count, 1);
}

TEST(DockerEnvironment, ResolveDigestReturnsEmptyOnFailure) {
    he::DockerEnvironment env;
    env.set_docker_runner([&](const std::vector<std::string>&) {
        he::CompletedProcess cp;
        cp.exit_code = 1;
        cp.stdout_text = "manifest unknown";
        return cp;
    });
    EXPECT_EQ(env.resolve_image_digest("missing:tag"), "");
}

TEST(DockerEnvironment, ResolveDigestFallsBackToBuildx) {
    he::DockerEnvironment env;
    int call = 0;
    env.set_docker_runner([&](const std::vector<std::string>& argv) {
        ++call;
        he::CompletedProcess cp;
        if (argv[0] == "manifest") {
            cp.exit_code = 1;
        } else {
            EXPECT_EQ(argv[0], "buildx");
            cp.exit_code = 0;
            cp.stdout_text = R"({"digest":"sha256:cafe0001"})";
        }
        return cp;
    });
    auto pinned = env.resolve_image_digest("foo:bar");
    EXPECT_EQ(pinned, "foo@sha256:cafe0001");
    EXPECT_EQ(call, 2);
}

TEST(DockerEnvironment, PinConfiguredImageRewritesConfig) {
    he::DockerEnvironment::Config config;
    config.image = "alpine:3.20";
    he::DockerEnvironment env(config);
    env.set_docker_runner([&](const std::vector<std::string>&) {
        he::CompletedProcess cp;
        cp.exit_code = 0;
        cp.stdout_text = R"({"digest":"sha256:aaaaaaaa"})";
        return cp;
    });
    EXPECT_TRUE(env.pin_configured_image());

    // Post-pin, build_docker_args should still work — and a second
    // resolve of the pinned form should NOT invoke the runner.
    int call_after = 0;
    env.set_docker_runner([&](const std::vector<std::string>&) {
        ++call_after;
        return he::CompletedProcess{};
    });
    auto again = env.resolve_image_digest("alpine@sha256:aaaaaaaa");
    EXPECT_EQ(again, "alpine@sha256:aaaaaaaa");
    EXPECT_EQ(call_after, 0);
}

TEST(DockerEnvironment, ImageWithRegistryPortSplitsCorrectly) {
    he::DockerEnvironment env;
    env.set_docker_runner([&](const std::vector<std::string>&) {
        he::CompletedProcess cp;
        cp.exit_code = 0;
        cp.stdout_text = R"({"digest":"sha256:0123"})";
        return cp;
    });
    auto pinned = env.resolve_image_digest("registry.local:5000/team/app:1.2");
    EXPECT_EQ(pinned, "registry.local:5000/team/app@sha256:0123");
}

// ---------------------------------------------------------------------------
// Anonymous volume cleanup.
// ---------------------------------------------------------------------------

TEST(DockerEnvironment, CleanupRemovesTrackedAnonymousVolumes) {
    he::DockerEnvironment env;
    std::vector<std::vector<std::string>> invocations;
    env.set_docker_runner([&](const std::vector<std::string>& argv) {
        invocations.push_back(argv);
        he::CompletedProcess cp;
        cp.exit_code = 0;
        return cp;
    });

    env.track_anonymous_volume("hermes-vol-1");
    env.track_anonymous_volume("hermes-vol-2");
    env.cleanup();

    ASSERT_EQ(invocations.size(), 2u);
    EXPECT_EQ(invocations[0][0], "volume");
    EXPECT_EQ(invocations[0][1], "rm");
    EXPECT_EQ(invocations[0][3], "hermes-vol-1");
    EXPECT_EQ(invocations[1][3], "hermes-vol-2");

    // Calling cleanup again should be a no-op (list was cleared).
    invocations.clear();
    env.cleanup();
    EXPECT_TRUE(invocations.empty());
}

TEST(DockerEnvironment, CleanupIgnoresEmptyVolumeIds) {
    he::DockerEnvironment env;
    int count = 0;
    env.set_docker_runner([&](const std::vector<std::string>&) {
        ++count;
        return he::CompletedProcess{};
    });
    env.track_anonymous_volume("");
    env.cleanup();
    EXPECT_EQ(count, 0);
}
