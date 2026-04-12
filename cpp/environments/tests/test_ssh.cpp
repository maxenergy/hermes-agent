#include "hermes/environments/ssh.hpp"

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

TEST(SSHEnvironment, BuildSshArgvControlMaster) {
    he::SSHEnvironment::Config config;
    config.host = "example.com";
    config.user = "testuser";
    he::SSHEnvironment env(config);

    auto argv = env.build_ssh_argv("echo hello");

    // Check ControlMaster flags.
    EXPECT_TRUE(has_arg(argv, "ControlMaster=auto"));
    // Check ControlPath contains the socket path.
    bool found_control_path = false;
    for (const auto& arg : argv) {
        if (arg.find("ControlPath=") != std::string::npos) {
            found_control_path = true;
            EXPECT_NE(arg.find("hermes-"), std::string::npos);
        }
    }
    EXPECT_TRUE(found_control_path);

    EXPECT_TRUE(has_arg(argv, "ControlPersist=60"));
}

TEST(SSHEnvironment, BuildSshArgvSocketPath) {
    he::SSHEnvironment::Config config;
    config.host = "myhost";
    config.user = "deploy";
    config.control_dir = "/tmp/hermes-ssh";
    he::SSHEnvironment env(config);

    auto argv = env.build_ssh_argv("ls");
    // The control path should contain user@host.
    bool found = false;
    for (const auto& arg : argv) {
        if (arg.find("/tmp/hermes-ssh/hermes-deploy@myhost") !=
            std::string::npos) {
            found = true;
        }
    }
    EXPECT_TRUE(found);
}

TEST(SSHEnvironment, BuildSshArgvPort) {
    he::SSHEnvironment::Config config;
    config.host = "example.com";
    config.user = "user";
    config.port = 2222;
    he::SSHEnvironment env(config);

    auto argv = env.build_ssh_argv("pwd");
    EXPECT_EQ(arg_after(argv, "-p"), "2222");
}

TEST(SSHEnvironment, BuildSshArgvIdentityFile) {
    he::SSHEnvironment::Config config;
    config.host = "example.com";
    config.user = "user";
    config.identity_file = "/home/user/.ssh/id_rsa";
    he::SSHEnvironment env(config);

    auto argv = env.build_ssh_argv("whoami");
    EXPECT_EQ(arg_after(argv, "-i"), "/home/user/.ssh/id_rsa");
}

TEST(SSHEnvironment, BuildSshArgvUserAtHost) {
    he::SSHEnvironment::Config config;
    config.host = "example.com";
    config.user = "deploy";
    he::SSHEnvironment env(config);

    auto argv = env.build_ssh_argv("ls");
    EXPECT_TRUE(has_arg(argv, "deploy@example.com"));
}

TEST(SSHEnvironment, BuildSshArgvCommand) {
    he::SSHEnvironment::Config config;
    config.host = "example.com";
    config.user = "user";
    he::SSHEnvironment env(config);

    auto argv = env.build_ssh_argv("echo test");
    EXPECT_EQ(argv.back(), "echo test");
}

// E2E test gated on SSH_TEST_HOST.
TEST(SSHEnvironment, E2E_EchoHello) {
    const char* ssh_host = std::getenv("SSH_TEST_HOST");
    if (!ssh_host) {
        GTEST_SKIP() << "Set SSH_TEST_HOST to run SSH E2E tests";
    }

    he::SSHEnvironment::Config config;
    config.host = ssh_host;
    const char* ssh_user = std::getenv("SSH_TEST_USER");
    if (ssh_user) config.user = ssh_user;

    he::SSHEnvironment env(config);
    he::ExecuteOptions opts;

    auto result = env.execute("echo hello", opts);
    EXPECT_EQ(result.exit_code, 0);
    EXPECT_NE(result.stdout_text.find("hello"), std::string::npos);
}
