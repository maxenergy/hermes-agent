#include "hermes/environments/env_filter.hpp"

#include <gtest/gtest.h>

namespace he = hermes::environments;

TEST(EnvFilter, SensitiveSuffixes) {
    EXPECT_TRUE(he::is_sensitive_var("MY_API_KEY"));
    EXPECT_TRUE(he::is_sensitive_var("SOME_TOKEN"));
    EXPECT_TRUE(he::is_sensitive_var("DB_SECRET"));
    EXPECT_TRUE(he::is_sensitive_var("USER_PASSWORD"));
    EXPECT_TRUE(he::is_sensitive_var("OAUTH_CLIENT_SECRET"));
}

TEST(EnvFilter, SensitiveSuffixCaseInsensitive) {
    EXPECT_TRUE(he::is_sensitive_var("my_api_key"));
    EXPECT_TRUE(he::is_sensitive_var("some_token"));
}

TEST(EnvFilter, ExplicitBlocklist) {
    EXPECT_TRUE(he::is_sensitive_var("ANTHROPIC_API_KEY"));
    EXPECT_TRUE(he::is_sensitive_var("OPENAI_API_KEY"));
    EXPECT_TRUE(he::is_sensitive_var("TELEGRAM_BOT_TOKEN"));
    EXPECT_TRUE(he::is_sensitive_var("DISCORD_BOT_TOKEN"));
    EXPECT_TRUE(he::is_sensitive_var("SLACK_BOT_TOKEN"));
    EXPECT_TRUE(he::is_sensitive_var("OPENROUTER_API_KEY"));
    EXPECT_TRUE(he::is_sensitive_var("AWS_SECRET_ACCESS_KEY"));
    EXPECT_TRUE(he::is_sensitive_var("GITHUB_TOKEN"));
    EXPECT_TRUE(he::is_sensitive_var("DATABASE_URL"));
    EXPECT_TRUE(he::is_sensitive_var("REDIS_URL"));
}

TEST(EnvFilter, HermesHomePasses) {
    // HERMES_HOME is intentionally not sensitive — child processes need
    // it to locate configuration directories.
    EXPECT_FALSE(he::is_sensitive_var("HERMES_HOME"));
}

TEST(EnvFilter, NormalVarsPass) {
    EXPECT_FALSE(he::is_sensitive_var("HOME"));
    EXPECT_FALSE(he::is_sensitive_var("PATH"));
    EXPECT_FALSE(he::is_sensitive_var("SHELL"));
    EXPECT_FALSE(he::is_sensitive_var("LANG"));
    EXPECT_FALSE(he::is_sensitive_var("USER"));
    EXPECT_FALSE(he::is_sensitive_var("TERM"));
}

TEST(EnvFilter, FilterEnvRemovesSensitive) {
    std::unordered_map<std::string, std::string> input = {
        {"HOME", "/home/user"},
        {"PATH", "/usr/bin"},
        {"OPENAI_API_KEY", "sk-secret"},
        {"MY_TOKEN", "tok-secret"},
        {"HERMES_HOME", "/opt/hermes"},
    };
    auto filtered = he::filter_env(input);

    EXPECT_EQ(filtered.count("HOME"), 1u);
    EXPECT_EQ(filtered.count("PATH"), 1u);
    EXPECT_EQ(filtered.count("HERMES_HOME"), 1u);
    EXPECT_EQ(filtered.count("OPENAI_API_KEY"), 0u);
    EXPECT_EQ(filtered.count("MY_TOKEN"), 0u);
}
