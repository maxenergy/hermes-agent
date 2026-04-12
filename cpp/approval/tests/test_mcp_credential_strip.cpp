#include "hermes/approval/mcp_credential_strip.hpp"

#include <gtest/gtest.h>

#include <string>

using hermes::approval::strip_credentials;

TEST(McpCredentialStrip, BearerTokenStripped) {
    // Token must be 20+ chars to match the redaction patterns.
    std::string input = "Authorization: Bearer sk-abc123def456ghijklmnopqrs";
    auto result = strip_credentials(input);
    EXPECT_EQ(result.find("sk-abc123def456ghijklmnopqrs"), std::string::npos);
    EXPECT_NE(result.find("REDACTED"), std::string::npos);
}

TEST(McpCredentialStrip, GithubTokenStripped) {
    std::string input = "token: ghp_abcdefghijklmnopqrstuvwx";
    auto result = strip_credentials(input);
    EXPECT_EQ(result.find("ghp_abcdefghijklmnopqrstuvwx"), std::string::npos);
}

TEST(McpCredentialStrip, NormalTextPreserved) {
    std::string input = "This is a normal log message with no secrets.";
    auto result = strip_credentials(input);
    EXPECT_EQ(result, input);
}

TEST(McpCredentialStrip, AwsKeyStripped) {
    std::string input = "key: AKIAIOSFODNN7EXAMPLE";
    auto result = strip_credentials(input);
    EXPECT_EQ(result.find("AKIAIOSFODNN7EXAMPLE"), std::string::npos);
}
