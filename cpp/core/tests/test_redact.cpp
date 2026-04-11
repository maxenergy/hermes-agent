#include "hermes/core/redact.hpp"

#include <gtest/gtest.h>
#include <string>

namespace hr = hermes::core::redact;

TEST(Redact, OpenAiKey) {
    const auto redacted = hr::redact_secrets("Authorization: sk-abcdefghijklmnopqrstuvwxyz012345");
    EXPECT_NE(redacted.find("***REDACTED***"), std::string::npos);
    EXPECT_EQ(redacted.find("sk-abcdefghijklmnopqrstuvwxyz012345"), std::string::npos);
}

TEST(Redact, GithubToken) {
    const auto redacted = hr::redact_secrets("X-GitHub-Token: ghp_1234567890abcdefghijKLMN");
    EXPECT_NE(redacted.find("***REDACTED***"), std::string::npos);
}

TEST(Redact, BearerToken) {
    const auto redacted = hr::redact_secrets("Authorization: Bearer eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9");
    EXPECT_NE(redacted.find("***REDACTED***"), std::string::npos);
    EXPECT_EQ(redacted.find("eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9"), std::string::npos);
}

TEST(Redact, QueryParamCredentials) {
    const auto redacted = hr::redact_secrets(
        "https://host/api?token=abcdef1234567890ABCDEF&other=keep "
        "password=literalpw secret=topSecretValue");
    // Each pattern replaces its whole match with the sentinel, so we
    // assert the sentinel is present and the original values are gone.
    const auto sentinel_count = [&]() {
        std::size_t n = 0;
        for (std::size_t pos = 0; (pos = redacted.find("***REDACTED***", pos)) != std::string::npos; ) {
            ++n;
            pos += 14;
        }
        return n;
    }();
    EXPECT_GE(sentinel_count, 3U);
    EXPECT_EQ(redacted.find("abcdef1234567890ABCDEF"), std::string::npos);
    EXPECT_EQ(redacted.find("literalpw"), std::string::npos);
    EXPECT_EQ(redacted.find("topSecretValue"), std::string::npos);
    // Unrelated params must survive untouched.
    EXPECT_NE(redacted.find("other=keep"), std::string::npos);
}

TEST(Redact, CleanInputUnchanged) {
    const std::string clean = "just a normal log line with no secrets";
    EXPECT_EQ(hr::redact_secrets(clean), clean);
}
