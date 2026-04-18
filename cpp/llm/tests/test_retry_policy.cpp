#include "hermes/llm/retry_policy.hpp"
#include "hermes/llm/error_classifier.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <set>

using hermes::llm::ClassifiedError;
using hermes::llm::FailoverReason;
using hermes::llm::RateLimitState;
using std::chrono::milliseconds;

TEST(RetryPolicy, BackoffReturnsRetryAfterWhenPresent) {
    ClassifiedError err;
    err.reason = FailoverReason::RateLimit;
    err.retry_after = std::chrono::seconds(7);
    const auto delay = hermes::llm::backoff_for_error(1, err);
    EXPECT_EQ(delay, milliseconds(7000));
}

TEST(RetryPolicy, BackoffFallsBackToJitteredBackoff) {
    ClassifiedError err;
    err.reason = FailoverReason::Unknown;
    const auto delay = hermes::llm::backoff_for_error(1, err);
    // Unknown reason -> core jittered_backoff (base 1s, cap 60s, ±25%).
    EXPECT_GT(delay.count(), 0);
    EXPECT_LE(delay.count(), 60'000);
}

// ── Per-tier attempt=1 windows ────────────────────────────────────────────
// Each tier uses a ±20% jitter around base * factor^(attempt-1).

TEST(RetryPolicy, RateLimitTierAttempt1) {
    // base=3s, factor=1.8, ±20% → [2400, 3600] ms.
    ClassifiedError err; err.reason = FailoverReason::RateLimit;
    for (int i = 0; i < 20; ++i) {
        const auto delay = hermes::llm::backoff_for_error(1, err);
        EXPECT_GE(delay.count(), 2400);
        EXPECT_LE(delay.count(), 3600);
    }
}

TEST(RetryPolicy, RateLimitTierAttempt2) {
    // base*factor=5.4s → [4320, 6480] ms.
    ClassifiedError err; err.reason = FailoverReason::RateLimit;
    for (int i = 0; i < 20; ++i) {
        const auto delay = hermes::llm::backoff_for_error(2, err);
        EXPECT_GE(delay.count(), 4320);
        EXPECT_LE(delay.count(), 6480);
    }
}

TEST(RetryPolicy, RateLimitTierCapsAt60s) {
    // attempt=10 → 3*1.8^9 ≈ 594s, capped to 60s → [48000, 72000] ms.
    ClassifiedError err; err.reason = FailoverReason::RateLimit;
    const auto delay = hermes::llm::backoff_for_error(10, err);
    EXPECT_GE(delay.count(), 48'000);
    EXPECT_LE(delay.count(), 72'000);
}

TEST(RetryPolicy, ServerErrorTier) {
    // base=1.5s, factor=2.0, cap=30s → att1 in [1200, 1800], att3 in [4800, 7200].
    ClassifiedError err; err.reason = FailoverReason::ServerError;
    const auto d1 = hermes::llm::backoff_for_error(1, err);
    EXPECT_GE(d1.count(), 1200);
    EXPECT_LE(d1.count(), 1800);
    const auto d3 = hermes::llm::backoff_for_error(3, err);
    EXPECT_GE(d3.count(), 4800);
    EXPECT_LE(d3.count(), 7200);
    const auto d10 = hermes::llm::backoff_for_error(10, err);
    // 1.5 * 2^9 = 768s → capped 30s → [24000, 36000].
    EXPECT_GE(d10.count(), 24'000);
    EXPECT_LE(d10.count(), 36'000);
}

TEST(RetryPolicy, NetworkErrorTier) {
    // base=1s, factor=2.0, cap=15s → att1 [800, 1200], att3 [3200, 4800].
    ClassifiedError err; err.reason = FailoverReason::NetworkError;
    const auto d1 = hermes::llm::backoff_for_error(1, err);
    EXPECT_GE(d1.count(), 800);
    EXPECT_LE(d1.count(), 1200);
    const auto d3 = hermes::llm::backoff_for_error(3, err);
    EXPECT_GE(d3.count(), 3200);
    EXPECT_LE(d3.count(), 4800);
}

TEST(RetryPolicy, TimeoutTier) {
    // base=2s, factor=1.5, cap=20s → att1 [1600, 2400], att2 [2400, 3600].
    ClassifiedError err; err.reason = FailoverReason::Timeout;
    const auto d1 = hermes::llm::backoff_for_error(1, err);
    EXPECT_GE(d1.count(), 1600);
    EXPECT_LE(d1.count(), 2400);
    const auto d2 = hermes::llm::backoff_for_error(2, err);
    EXPECT_GE(d2.count(), 2400);
    EXPECT_LE(d2.count(), 3600);
}

TEST(RetryPolicy, TieredBackoffHasVariance) {
    // Jitter should actually vary — 20 samples shouldn't all collapse to
    // the same millisecond.
    ClassifiedError err; err.reason = FailoverReason::ServerError;
    std::set<long long> seen;
    for (int i = 0; i < 20; ++i) {
        seen.insert(hermes::llm::backoff_for_error(1, err).count());
    }
    EXPECT_GT(seen.size(), 1u);
}

// ── Rate-limit header parsing (existing coverage) ────────────────────────

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
