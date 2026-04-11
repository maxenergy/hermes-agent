#include "hermes/llm/retry_policy.hpp"
#include "hermes/llm/error_classifier.hpp"

#include <gtest/gtest.h>

using hermes::llm::ClassifiedError;
using hermes::llm::FailoverReason;
using hermes::llm::RateLimitState;

TEST(RetryPolicy, BackoffReturnsRetryAfterWhenPresent) {
    ClassifiedError err;
    err.reason = FailoverReason::RateLimit;
    err.retry_after = std::chrono::seconds(7);
    const auto delay = hermes::llm::backoff_for_error(1, err);
    EXPECT_EQ(delay, std::chrono::milliseconds(7000));
}

TEST(RetryPolicy, BackoffFallsBackToJitteredBackoff) {
    ClassifiedError err;
    err.reason = FailoverReason::ServerError;
    const auto delay = hermes::llm::backoff_for_error(1, err);
    // Base is 1s with 25% jitter; must be > 0 and < max 60s.
    EXPECT_GT(delay.count(), 0);
    EXPECT_LE(delay.count(), 60'000);
}

TEST(RateLimit, UpdatesRemainingAndResetFromHeaders) {
    RateLimitState st;
    std::unordered_map<std::string, std::string> headers = {
        {"x-ratelimit-remaining-requests", "5"},
        {"x-ratelimit-remaining-tokens", "20000"},
        {"x-ratelimit-reset-after", "30"},
    };
    st.update_from_headers(headers);
    ASSERT_TRUE(st.remaining_requests.has_value());
    EXPECT_EQ(*st.remaining_requests, 5);
    ASSERT_TRUE(st.remaining_tokens.has_value());
    EXPECT_EQ(*st.remaining_tokens, 20000);
    ASSERT_TRUE(st.reset_at.has_value());
    EXPECT_FALSE(st.should_throttle());
}

TEST(RateLimit, ShouldThrottleWhenExhausted) {
    RateLimitState st;
    std::unordered_map<std::string, std::string> headers = {
        {"x-ratelimit-remaining-requests", "0"},
    };
    st.update_from_headers(headers);
    EXPECT_TRUE(st.should_throttle());
}

TEST(RateLimit, HandlesAlternativeResetHeaderVariants) {
    RateLimitState st;
    std::unordered_map<std::string, std::string> headers = {
        {"x-ratelimit-reset", "60"},
    };
    st.update_from_headers(headers);
    EXPECT_TRUE(st.reset_at.has_value());
}
