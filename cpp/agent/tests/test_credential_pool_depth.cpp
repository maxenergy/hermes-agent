// gtest cases for credential_pool_depth.cpp.  Mirrors the Python unit
// expectations under tests/test_credential_pool.py for the pure
// helpers.

#include "hermes/agent/credential_pool_depth.hpp"

#include <gtest/gtest.h>

#include <cmath>

using namespace hermes::agent::creds::depth;

namespace {

bool near(double a, double b, double eps = 1e-3) {
    return std::fabs(a - b) < eps;
}

}  // namespace

TEST(CredentialPoolDepth, IsSupportedStrategy) {
    EXPECT_TRUE(is_supported_strategy("fill_first"));
    EXPECT_TRUE(is_supported_strategy("round_robin"));
    EXPECT_TRUE(is_supported_strategy("random"));
    EXPECT_TRUE(is_supported_strategy("least_used"));
    EXPECT_TRUE(is_supported_strategy("FILL_FIRST"));
    EXPECT_FALSE(is_supported_strategy("priority"));
    EXPECT_FALSE(is_supported_strategy(""));
}

TEST(CredentialPoolDepth, ExhaustedTtlDefault) {
    EXPECT_EQ(exhausted_ttl(429), 3600);
    EXPECT_EQ(exhausted_ttl(402), 3600);
    EXPECT_EQ(exhausted_ttl(0), 3600);
    EXPECT_EQ(exhausted_ttl(500), 3600);
}

TEST(CredentialPoolDepth, ParseNumericSeconds) {
    auto v = parse_absolute_timestamp_numeric(1700000000.0);
    ASSERT_TRUE(v.has_value());
    EXPECT_DOUBLE_EQ(*v, 1700000000.0);
}

TEST(CredentialPoolDepth, ParseNumericMilliseconds) {
    auto v = parse_absolute_timestamp_numeric(1700000000000.0);
    ASSERT_TRUE(v.has_value());
    EXPECT_DOUBLE_EQ(*v, 1700000000.0);
}

TEST(CredentialPoolDepth, ParseNumericRejectsZeroAndNegative) {
    EXPECT_FALSE(parse_absolute_timestamp_numeric(0.0).has_value());
    EXPECT_FALSE(parse_absolute_timestamp_numeric(-5.0).has_value());
}

TEST(CredentialPoolDepth, ParseStringNumeric) {
    auto v = parse_absolute_timestamp_string("  1700000000  ");
    ASSERT_TRUE(v.has_value());
    EXPECT_DOUBLE_EQ(*v, 1700000000.0);
}

TEST(CredentialPoolDepth, ParseStringMilliseconds) {
    auto v = parse_absolute_timestamp_string("1700000000000");
    ASSERT_TRUE(v.has_value());
    EXPECT_DOUBLE_EQ(*v, 1700000000.0);
}

TEST(CredentialPoolDepth, ParseStringIso8601Z) {
    auto v = parse_absolute_timestamp_string("2024-01-01T00:00:00Z");
    ASSERT_TRUE(v.has_value());
    // 2024-01-01T00:00:00 UTC = 1704067200
    EXPECT_DOUBLE_EQ(*v, 1704067200.0);
}

TEST(CredentialPoolDepth, ParseStringIso8601Offset) {
    // 2024-01-01T08:00:00+08:00 == 2024-01-01T00:00:00Z
    auto v = parse_absolute_timestamp_string("2024-01-01T08:00:00+08:00");
    ASSERT_TRUE(v.has_value());
    EXPECT_DOUBLE_EQ(*v, 1704067200.0);
}

TEST(CredentialPoolDepth, ParseStringEmptyReturnsNullopt) {
    EXPECT_FALSE(parse_absolute_timestamp_string("").has_value());
    EXPECT_FALSE(parse_absolute_timestamp_string("   ").has_value());
    EXPECT_FALSE(parse_absolute_timestamp_string("not-a-time").has_value());
}

TEST(CredentialPoolDepth, RetryDelayMilliseconds) {
    auto v = extract_retry_delay_seconds("quotaResetDelay:\"1500ms\"");
    ASSERT_TRUE(v.has_value());
    EXPECT_DOUBLE_EQ(*v, 1.5);
}

TEST(CredentialPoolDepth, RetryDelaySecondsSuffix) {
    auto v = extract_retry_delay_seconds("quotaResetDelay: 42s");
    ASSERT_TRUE(v.has_value());
    EXPECT_DOUBLE_EQ(*v, 42.0);
}

TEST(CredentialPoolDepth, RetryAfterSeconds) {
    auto v = extract_retry_delay_seconds("please retry after 30 seconds");
    ASSERT_TRUE(v.has_value());
    EXPECT_DOUBLE_EQ(*v, 30.0);
}

TEST(CredentialPoolDepth, RetryBareS) {
    auto v = extract_retry_delay_seconds("retry 12s please");
    ASSERT_TRUE(v.has_value());
    EXPECT_DOUBLE_EQ(*v, 12.0);
}

TEST(CredentialPoolDepth, RetryDelayNone) {
    EXPECT_FALSE(extract_retry_delay_seconds("").has_value());
    EXPECT_FALSE(extract_retry_delay_seconds("rate limit exceeded").has_value());
}

TEST(CredentialPoolDepth, NormalizeErrorBasic) {
    RawErrorContext raw;
    raw.reason = "  rate_limit  ";
    raw.message = "  too fast  ";
    raw.reset_at_str = "1700000000";
    auto n = normalize_error_context(raw, /*now=*/1000.0);
    ASSERT_TRUE(n.reason.has_value());
    EXPECT_EQ(*n.reason, "rate_limit");
    ASSERT_TRUE(n.message.has_value());
    EXPECT_EQ(*n.message, "too fast");
    ASSERT_TRUE(n.reset_at.has_value());
    EXPECT_DOUBLE_EQ(*n.reset_at, 1700000000.0);
}

TEST(CredentialPoolDepth, NormalizeErrorFallback) {
    RawErrorContext raw;
    raw.retry_until_str = "1700000500";
    auto n = normalize_error_context(raw, 0.0);
    ASSERT_TRUE(n.reset_at.has_value());
    EXPECT_DOUBLE_EQ(*n.reset_at, 1700000500.0);
}

TEST(CredentialPoolDepth, NormalizeErrorFromRetryDelay) {
    RawErrorContext raw;
    raw.message = "retry after 120 seconds";
    auto n = normalize_error_context(raw, /*now=*/1000.0);
    ASSERT_TRUE(n.reset_at.has_value());
    EXPECT_DOUBLE_EQ(*n.reset_at, 1120.0);
}

TEST(CredentialPoolDepth, NormalizeErrorEmpty) {
    RawErrorContext raw;
    auto n = normalize_error_context(raw, 0.0);
    EXPECT_FALSE(n.reason.has_value());
    EXPECT_FALSE(n.message.has_value());
    EXPECT_FALSE(n.reset_at.has_value());
}

TEST(CredentialPoolDepth, NormalizeCustomPoolName) {
    EXPECT_EQ(normalize_custom_pool_name("  DeepSeek  "), "deepseek");
    EXPECT_EQ(normalize_custom_pool_name("Together AI"), "together-ai");
    EXPECT_EQ(normalize_custom_pool_name("multi word entry"), "multi-word-entry");
}

TEST(CredentialPoolDepth, CustomProviderPoolKeyMatch) {
    std::vector<CustomProviderEntry> providers = {
        {"deepseek", "https://api.deepseek.com/v1"},
        {"together-ai", "https://api.together.ai"},
    };
    auto k = custom_provider_pool_key("https://api.deepseek.com/v1/",
                                      providers);
    ASSERT_TRUE(k.has_value());
    EXPECT_EQ(*k, "custom:deepseek");
}

TEST(CredentialPoolDepth, CustomProviderPoolKeyNoMatch) {
    std::vector<CustomProviderEntry> providers = {
        {"deepseek", "https://api.deepseek.com/v1"},
    };
    EXPECT_FALSE(custom_provider_pool_key("https://api.openai.com/v1",
                                          providers).has_value());
    EXPECT_FALSE(custom_provider_pool_key("", providers).has_value());
}

TEST(CredentialPoolDepth, ListCustomPoolProvidersSorts) {
    std::vector<std::pair<std::string, std::size_t>> pool = {
        {"custom:zz-last", 2},
        {"custom:aa-first", 1},
        {"openai", 3},
        {"custom:empty", 0},
        {"custom:middle", 1},
    };
    auto out = list_custom_pool_providers(pool);
    ASSERT_EQ(out.size(), 3u);
    EXPECT_EQ(out[0], "custom:aa-first");
    EXPECT_EQ(out[1], "custom:middle");
    EXPECT_EQ(out[2], "custom:zz-last");
}

TEST(CredentialPoolDepth, IsManualSource) {
    EXPECT_TRUE(is_manual_source("manual"));
    EXPECT_TRUE(is_manual_source("MANUAL"));
    EXPECT_TRUE(is_manual_source("manual:extra"));
    EXPECT_TRUE(is_manual_source("  Manual:tag  "));
    EXPECT_FALSE(is_manual_source("device_code"));
    EXPECT_FALSE(is_manual_source(""));
    EXPECT_FALSE(is_manual_source("manually"));
}

TEST(CredentialPoolDepth, NextPriority) {
    EXPECT_EQ(next_priority({}), 0);
    EXPECT_EQ(next_priority({0}), 1);
    EXPECT_EQ(next_priority({0, 2, 5, 3}), 6);
}

TEST(CredentialPoolDepth, LabelFromClaimsEmailWins) {
    std::unordered_map<std::string, std::string> claims = {
        {"email", "user@example.com"},
        {"preferred_username", "other"},
    };
    EXPECT_EQ(label_from_claims(claims, "fallback"), "user@example.com");
}

TEST(CredentialPoolDepth, LabelFromClaimsFallback) {
    std::unordered_map<std::string, std::string> claims = {
        {"sub", "abc123"},
    };
    EXPECT_EQ(label_from_claims(claims, "user@nous"), "user@nous");
}

TEST(CredentialPoolDepth, LabelFromClaimsSkipsEmptyEmail) {
    std::unordered_map<std::string, std::string> claims = {
        {"email", "   "},
        {"preferred_username", "pref"},
    };
    EXPECT_EQ(label_from_claims(claims, "fb"), "pref");
}

TEST(CredentialPoolDepth, ExhaustedUntilFromResetAt) {
    EntrySnapshot e;
    e.last_status = "exhausted";
    e.last_error_reset_at = 1700000500.0;
    auto u = exhausted_until(e);
    ASSERT_TRUE(u.has_value());
    EXPECT_DOUBLE_EQ(*u, 1700000500.0);
}

TEST(CredentialPoolDepth, ExhaustedUntilFromStatusAtAndCode) {
    EntrySnapshot e;
    e.last_status = "exhausted";
    e.last_status_at = 1000.0;
    e.last_error_code = 429;
    auto u = exhausted_until(e);
    ASSERT_TRUE(u.has_value());
    EXPECT_DOUBLE_EQ(*u, 1000.0 + 3600.0);
}

TEST(CredentialPoolDepth, ExhaustedUntilNoneWhenOk) {
    EntrySnapshot e;
    e.last_status = "ok";
    e.last_status_at = 1000.0;
    EXPECT_FALSE(exhausted_until(e).has_value());
}

TEST(CredentialPoolDepth, NumericResetAtViaMilliseconds) {
    EntrySnapshot e;
    e.last_status = "exhausted";
    e.last_error_reset_at = 1700000500000.0;  // ms
    auto u = exhausted_until(e);
    ASSERT_TRUE(u.has_value());
    EXPECT_TRUE(near(*u, 1700000500.0));
}
