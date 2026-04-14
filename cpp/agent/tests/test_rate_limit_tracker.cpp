// Tests for hermes::agent::RateLimitState / parser / formatters.
#include "hermes/agent/rate_limit_tracker.hpp"

#include <gtest/gtest.h>

using hermes::agent::RateLimitBucket;
using hermes::agent::RateLimitState;
using hermes::agent::parse_rate_limit_headers;
using hermes::agent::format_rate_limit_display;
using hermes::agent::format_rate_limit_compact;

TEST(RateLimitBucket, UsedAndPct) {
    RateLimitBucket b;
    b.limit = 100;
    b.remaining = 40;
    EXPECT_EQ(b.used(), 60);
    EXPECT_DOUBLE_EQ(b.usage_pct(), 60.0);
}

TEST(RateLimitBucket, UsedClampedWhenRemainingExceedsLimit) {
    RateLimitBucket b;
    b.limit = 10;
    b.remaining = 20;
    EXPECT_EQ(b.used(), 0);
}

TEST(RateLimitBucket, UsagePctZeroLimit) {
    RateLimitBucket b;
    EXPECT_DOUBLE_EQ(b.usage_pct(), 0.0);
}

TEST(RateLimitBucket, RemainingSecondsSubtractsElapsed) {
    RateLimitBucket b;
    b.captured_at = 100.0;
    b.reset_seconds = 30.0;
    EXPECT_DOUBLE_EQ(b.remaining_seconds_now(110.0), 20.0);
    EXPECT_DOUBLE_EQ(b.remaining_seconds_now(140.0), 0.0);
}

TEST(ParseRateLimitHeaders, NoHeadersReturnsNullopt) {
    std::unordered_map<std::string, std::string> h{
        {"content-type", "application/json"},
    };
    EXPECT_FALSE(parse_rate_limit_headers(h).has_value());
}

TEST(ParseRateLimitHeaders, ParsesAllTwelveHeaders) {
    std::unordered_map<std::string, std::string> h{
        {"x-ratelimit-limit-requests", "60"},
        {"x-ratelimit-remaining-requests", "20"},
        {"x-ratelimit-reset-requests", "30"},
        {"x-ratelimit-limit-requests-1h", "1000"},
        {"x-ratelimit-remaining-requests-1h", "200"},
        {"x-ratelimit-reset-requests-1h", "1200"},
        {"x-ratelimit-limit-tokens", "100000"},
        {"x-ratelimit-remaining-tokens", "40000"},
        {"x-ratelimit-reset-tokens", "30"},
        {"x-ratelimit-limit-tokens-1h", "8000000"},
        {"x-ratelimit-remaining-tokens-1h", "500000"},
        {"x-ratelimit-reset-tokens-1h", "3000"},
    };
    auto s = parse_rate_limit_headers(h, "nous");
    ASSERT_TRUE(s.has_value());
    EXPECT_EQ(s->provider, "nous");
    EXPECT_EQ(s->requests_min.limit, 60);
    EXPECT_EQ(s->requests_min.remaining, 20);
    EXPECT_DOUBLE_EQ(s->requests_min.reset_seconds, 30.0);
    EXPECT_EQ(s->requests_hour.limit, 1000);
    EXPECT_EQ(s->tokens_min.limit, 100000);
    EXPECT_EQ(s->tokens_hour.limit, 8000000);
    EXPECT_TRUE(s->has_data());
}

TEST(ParseRateLimitHeaders, CaseInsensitive) {
    std::unordered_map<std::string, std::string> h{
        {"X-RateLimit-Limit-Requests", "60"},
        {"X-RATELIMIT-REMAINING-REQUESTS", "30"},
    };
    auto s = parse_rate_limit_headers(h);
    ASSERT_TRUE(s.has_value());
    EXPECT_EQ(s->requests_min.limit, 60);
    EXPECT_EQ(s->requests_min.remaining, 30);
}

TEST(ParseRateLimitHeaders, GracefullyHandlesBadValues) {
    std::unordered_map<std::string, std::string> h{
        {"x-ratelimit-limit-requests", "not-a-number"},
        {"x-ratelimit-remaining-requests", ""},
    };
    auto s = parse_rate_limit_headers(h);
    ASSERT_TRUE(s.has_value());
    EXPECT_EQ(s->requests_min.limit, 0);
    EXPECT_EQ(s->requests_min.remaining, 0);
}

TEST(FmtCount, Formats) {
    using hermes::agent::detail::fmt_count;
    EXPECT_EQ(fmt_count(799), "799");
    EXPECT_EQ(fmt_count(33599), "33.6K");
    EXPECT_EQ(fmt_count(7999856), "8.0M");
}

TEST(FmtSeconds, Formats) {
    using hermes::agent::detail::fmt_seconds;
    EXPECT_EQ(fmt_seconds(45), "45s");
    EXPECT_EQ(fmt_seconds(60), "1m");
    EXPECT_EQ(fmt_seconds(134), "2m 14s");
    EXPECT_EQ(fmt_seconds(3600), "1h");
    EXPECT_EQ(fmt_seconds(3720), "1h 2m");
    EXPECT_EQ(fmt_seconds(-5), "0s");
}

TEST(Bar, ClampsAtBounds) {
    using hermes::agent::detail::bar;
    EXPECT_NE(bar(0).find('['), std::string::npos);
    EXPECT_NE(bar(120).find(']'), std::string::npos);
    EXPECT_NE(bar(50).size(), 0u);
}

TEST(FormatRateLimitDisplay, NoDataMessage) {
    RateLimitState empty;
    EXPECT_NE(format_rate_limit_display(empty).find("No rate limit data"),
              std::string::npos);
}

TEST(FormatRateLimitDisplay, IncludesProviderAndBuckets) {
    RateLimitState s;
    s.provider = "nous";
    s.captured_at = 1000.0;
    s.requests_min.limit = 60;
    s.requests_min.remaining = 10;
    s.requests_min.captured_at = 1000.0;
    s.requests_min.reset_seconds = 30.0;
    const std::string out = format_rate_limit_display(s, 1001.0);
    EXPECT_NE(out.find("Nous"), std::string::npos);
    EXPECT_NE(out.find("Requests/min"), std::string::npos);
    EXPECT_NE(out.find("⚠"), std::string::npos);  // 83% > 80 threshold
}

TEST(FormatRateLimitCompact, EmptyState) {
    RateLimitState s;
    EXPECT_EQ(format_rate_limit_compact(s), "No rate limit data.");
}

TEST(FormatRateLimitCompact, BuildsParts) {
    RateLimitState s;
    s.captured_at = 100.0;
    s.requests_min.limit = 60;
    s.requests_min.remaining = 20;
    s.requests_min.captured_at = 100.0;
    s.tokens_min.limit = 100000;
    s.tokens_min.remaining = 50000;
    s.tokens_min.captured_at = 100.0;
    const std::string out = format_rate_limit_compact(s, 100.0);
    EXPECT_NE(out.find("RPM: 20/60"), std::string::npos);
    EXPECT_NE(out.find("TPM:"), std::string::npos);
    EXPECT_NE(out.find(" | "), std::string::npos);
}
