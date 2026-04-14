// Tests for hermes::cli::uninstall_cmd helpers.
#include "hermes/cli/uninstall_cmd.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>

using namespace hermes::cli::uninstall_cmd;
namespace fs = std::filesystem;

TEST(UninstallCmd, ScrubShellConfig_RemovesHermesPathLine) {
    std::string src =
        "export PATH=$PATH:/opt/other\n"
        "# Hermes Agent\n"
        "export PATH=\"$PATH:$HOME/.hermes-agent/bin\"\n"
        "export EDITOR=vim\n";
    auto r = scrub_shell_config(src);
    EXPECT_TRUE(r.changed);
    EXPECT_EQ(r.content.find("hermes-agent"), std::string::npos);
    EXPECT_NE(r.content.find("EDITOR=vim"), std::string::npos);
    EXPECT_NE(r.content.find("/opt/other"), std::string::npos);
}

TEST(UninstallCmd, ScrubShellConfig_NoHermes_IsNoOp) {
    std::string src = "export FOO=bar\nexport PATH=/usr/bin\n";
    auto r = scrub_shell_config(src);
    EXPECT_FALSE(r.changed);
}

TEST(UninstallCmd, ScrubShellConfig_MultipleBlankLinesCompacted) {
    std::string src =
        "line1\n"
        "# Hermes Agent\n"
        "hermes PATH=/x\n"
        "\n\n\n"
        "line2\n";
    auto r = scrub_shell_config(src);
    EXPECT_TRUE(r.changed);
    EXPECT_EQ(r.content.find("\n\n\n"), std::string::npos);
}

TEST(UninstallCmd, IsHermesWrapper_TruePositives) {
    EXPECT_TRUE(is_hermes_wrapper("#!/usr/bin/env python3\n"
                                  "from hermes_cli.main import main"));
    EXPECT_TRUE(is_hermes_wrapper("python hermes-agent/run_agent.py"));
    EXPECT_TRUE(is_hermes_wrapper("exec hermes_cpp \"$@\""));
}

TEST(UninstallCmd, IsHermesWrapper_Negative) {
    EXPECT_FALSE(is_hermes_wrapper("#!/bin/sh\nexec ls"));
    EXPECT_FALSE(is_hermes_wrapper(""));
}

TEST(UninstallCmd, FindShellConfigs_OnlyReturnsExisting) {
    auto tmp = fs::temp_directory_path() / "uninstall_test_home";
    fs::create_directories(tmp);
    // Create only .bashrc.
    {
        std::ofstream f(tmp / ".bashrc");
        f << "PATH=$PATH\n";
    }
    auto configs = find_shell_configs(tmp);
    ASSERT_EQ(configs.size(), 1u);
    EXPECT_EQ(configs[0].filename(), ".bashrc");
    fs::remove_all(tmp);
}
