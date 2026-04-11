#include "hermes/core/env.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <string>

namespace env = hermes::core::env;
namespace fs = std::filesystem;

TEST(Env, TruthyValues) {
    EXPECT_TRUE(env::is_truthy_value("1"));
    EXPECT_TRUE(env::is_truthy_value("true"));
    EXPECT_TRUE(env::is_truthy_value("TRUE"));
    EXPECT_TRUE(env::is_truthy_value("Yes"));
    EXPECT_TRUE(env::is_truthy_value("on"));
    EXPECT_FALSE(env::is_truthy_value("0"));
    EXPECT_FALSE(env::is_truthy_value("no"));
    EXPECT_FALSE(env::is_truthy_value(""));
}

TEST(Env, EnvVarEnabled) {
    ::unsetenv("HERMES_TEST_VAR");
    EXPECT_FALSE(env::env_var_enabled("HERMES_TEST_VAR"));

    ::setenv("HERMES_TEST_VAR", "true", 1);
    EXPECT_TRUE(env::env_var_enabled("HERMES_TEST_VAR"));

    ::setenv("HERMES_TEST_VAR", "maybe", 1);
    EXPECT_FALSE(env::env_var_enabled("HERMES_TEST_VAR"));

    ::unsetenv("HERMES_TEST_VAR");
}

TEST(Env, LoadDotenvHappyPath) {
    auto tmp = fs::temp_directory_path() / "hermes-dotenv-happy";
    {
        std::ofstream out(tmp);
        out << "# a comment\n"
            << "HERMES_FOO=bar\n"
            << "HERMES_QUOTED=\"hello world\"\n"
            << "HERMES_SQUOTED='literal $NOT_EXPANDED'\n";
    }
    ::unsetenv("HERMES_FOO");
    ::unsetenv("HERMES_QUOTED");
    ::unsetenv("HERMES_SQUOTED");

    env::load_dotenv(tmp);
    EXPECT_STREQ(std::getenv("HERMES_FOO"), "bar");
    EXPECT_STREQ(std::getenv("HERMES_QUOTED"), "hello world");
    EXPECT_STREQ(std::getenv("HERMES_SQUOTED"), "literal $NOT_EXPANDED");

    ::unsetenv("HERMES_FOO");
    ::unsetenv("HERMES_QUOTED");
    ::unsetenv("HERMES_SQUOTED");
    fs::remove(tmp);
}

TEST(Env, LoadDotenvPreservesExistingVars) {
    auto tmp = fs::temp_directory_path() / "hermes-dotenv-preserve";
    {
        std::ofstream out(tmp);
        out << "HERMES_KEEP=from_file\n";
    }
    ::setenv("HERMES_KEEP", "from_caller", 1);
    env::load_dotenv(tmp);
    EXPECT_STREQ(std::getenv("HERMES_KEEP"), "from_caller");

    ::unsetenv("HERMES_KEEP");
    fs::remove(tmp);
}

TEST(Env, LoadDotenvMissingFileIsSilent) {
    env::load_dotenv(fs::temp_directory_path() / "definitely-not-there.env");
    SUCCEED();
}

TEST(Env, LoadDotenvExpandsExistingVars) {
    auto tmp = fs::temp_directory_path() / "hermes-dotenv-expand";
    {
        std::ofstream out(tmp);
        out << "HERMES_EXPANDED=${HERMES_BASE}/sub\n";
    }
    ::setenv("HERMES_BASE", "/root/dir", 1);
    ::unsetenv("HERMES_EXPANDED");
    env::load_dotenv(tmp);
    EXPECT_STREQ(std::getenv("HERMES_EXPANDED"), "/root/dir/sub");

    ::unsetenv("HERMES_BASE");
    ::unsetenv("HERMES_EXPANDED");
    fs::remove(tmp);
}
