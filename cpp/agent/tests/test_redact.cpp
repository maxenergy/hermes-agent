// Tests for hermes::agent::redact.
#include "hermes/agent/redact.hpp"

#include <cstdlib>
#include <gtest/gtest.h>

using hermes::agent::redact::mask_token;
using hermes::agent::redact::redact_sensitive_text;
using hermes::agent::redact::reset_enabled_cache_for_testing;

namespace {

// Ensure redaction is enabled at the start of each test (unset env var).
class RedactTest : public ::testing::Test {
protected:
    void SetUp() override {
        ::unsetenv("HERMES_REDACT_SECRETS");
        reset_enabled_cache_for_testing();
    }
    void TearDown() override {
        ::unsetenv("HERMES_REDACT_SECRETS");
        reset_enabled_cache_for_testing();
    }
};

}  // namespace

TEST_F(RedactTest, MaskShortToken) {
    EXPECT_EQ(mask_token("short"), "***");
    EXPECT_EQ(mask_token("0123456789abcdef0"), "***");  // 17 chars
}

TEST_F(RedactTest, MaskLongTokenPreservesPrefixSuffix) {
    const std::string tok = "sk-abcdef12345678901234";  // 23 chars
    const std::string masked = mask_token(tok);
    EXPECT_EQ(masked.substr(0, 6), tok.substr(0, 6));
    EXPECT_NE(masked.find("..."), std::string::npos);
    EXPECT_EQ(masked.substr(masked.size() - 4), tok.substr(tok.size() - 4));
}

TEST_F(RedactTest, RedactsOpenAIKey) {
    const std::string input = "key=sk-abcdefghij1234567890";
    const std::string out = redact_sensitive_text(input);
    EXPECT_EQ(out.find("sk-abcdefghij1234567890"), std::string::npos);
}

TEST_F(RedactTest, RedactsGitHubPAT) {
    const std::string input = "token ghp_abcdefghijklmnop1234";
    const std::string out = redact_sensitive_text(input);
    EXPECT_EQ(out.find("ghp_abcdefghijklmnop1234"), std::string::npos);
}

TEST_F(RedactTest, RedactsEnvAssignment) {
    const std::string input = "OPENAI_API_KEY=sk-abcdefghij1234567890";
    const std::string out = redact_sensitive_text(input);
    EXPECT_NE(out.find("OPENAI_API_KEY="), std::string::npos);
    EXPECT_EQ(out.find("sk-abcdefghij1234567890"), std::string::npos);
}

TEST_F(RedactTest, RedactsAuthHeader) {
    const std::string input = "Authorization: Bearer abc123def456ghi789";
    const std::string out = redact_sensitive_text(input);
    EXPECT_NE(out.find("Authorization: Bearer "), std::string::npos);
    EXPECT_EQ(out.find("abc123def456ghi789"), std::string::npos);
}

TEST_F(RedactTest, RedactsTelegramToken) {
    const std::string input = "bot123456789:ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
    const std::string out = redact_sensitive_text(input);
    EXPECT_NE(out.find(":***"), std::string::npos);
    EXPECT_EQ(out.find("ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789"), std::string::npos);
}

TEST_F(RedactTest, RedactsPrivateKey) {
    const std::string input =
        "before\n-----BEGIN RSA PRIVATE KEY-----\nMIIBOgIBAAJBAL+k\n-----END RSA PRIVATE KEY-----\nafter";
    const std::string out = redact_sensitive_text(input);
    EXPECT_NE(out.find("[REDACTED PRIVATE KEY]"), std::string::npos);
    EXPECT_EQ(out.find("MIIBOgIBAAJBAL+k"), std::string::npos);
}

TEST_F(RedactTest, RedactsDatabaseConnString) {
    const std::string input = "postgres://user:supersecretpass@localhost:5432/db";
    const std::string out = redact_sensitive_text(input);
    EXPECT_NE(out.find("postgres://user:***@"), std::string::npos);
    EXPECT_EQ(out.find("supersecretpass"), std::string::npos);
}

TEST_F(RedactTest, RedactsJsonField) {
    const std::string input = "{\"api_key\": \"sk-abcdef1234567890abc\"}";
    const std::string out = redact_sensitive_text(input);
    EXPECT_EQ(out.find("sk-abcdef1234567890abc"), std::string::npos);
}

TEST_F(RedactTest, PassthroughWhenDisabled) {
    ::setenv("HERMES_REDACT_SECRETS", "false", 1);
    reset_enabled_cache_for_testing();
    const std::string input = "OPENAI_API_KEY=sk-abcdefghij1234567890";
    EXPECT_EQ(redact_sensitive_text(input), input);
}

TEST_F(RedactTest, EmptyInputReturnsEmpty) {
    EXPECT_EQ(redact_sensitive_text(""), "");
}
