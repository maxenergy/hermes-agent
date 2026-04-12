#include "hermes/tools/env_passthrough.hpp"

#include <gtest/gtest.h>

using hermes::tools::is_sensitive_env_var;
using hermes::tools::safe_env_subset;

TEST(EnvPassthrough, ApiKeyIsSensitive) {
    EXPECT_TRUE(is_sensitive_env_var("OPENROUTER_API_KEY"));
}

TEST(EnvPassthrough, OpenaiPrefixSensitive) {
    EXPECT_TRUE(is_sensitive_env_var("OPENAI_API_KEY"));
}

TEST(EnvPassthrough, HermesPrefixSensitive) {
    EXPECT_TRUE(is_sensitive_env_var("HERMES_SECRET"));
}

TEST(EnvPassthrough, RegularVarNotSensitive) {
    EXPECT_FALSE(is_sensitive_env_var("PATH"));
    EXPECT_FALSE(is_sensitive_env_var("HOME"));
    EXPECT_FALSE(is_sensitive_env_var("LANG"));
}

TEST(EnvPassthrough, SafeEnvSubsetExcludesSensitive) {
    auto env = safe_env_subset();
    // The subset should not contain any variable that is_sensitive_env_var
    // would flag. Check a few common sensitive ones.
    EXPECT_EQ(env.count("OPENAI_API_KEY"), 0u);
    EXPECT_EQ(env.count("ANTHROPIC_API_KEY"), 0u);
}
