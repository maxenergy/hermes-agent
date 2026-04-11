#include "hermes/llm/error_classifier.hpp"

#include <gtest/gtest.h>

#include <unordered_map>

using hermes::llm::classify_api_error;
using hermes::llm::FailoverReason;

TEST(ErrorClassifier, RateLimit429WithRetryAfter) {
    std::unordered_map<std::string, std::string> headers = {
        {"Retry-After", "42"},
    };
    const auto err = classify_api_error(429, "{\"error\":\"slow down\"}", headers);
    EXPECT_EQ(err.reason, FailoverReason::RateLimit);
    ASSERT_TRUE(err.retry_after.has_value());
    EXPECT_EQ(err.retry_after->count(), 42);
}

TEST(ErrorClassifier, RateLimit429WithRetryAfterMs) {
    std::unordered_map<std::string, std::string> headers = {
        {"retry-after-ms", "12000"},
    };
    const auto err = classify_api_error(429, "rate limit", headers);
    EXPECT_EQ(err.reason, FailoverReason::RateLimit);
    ASSERT_TRUE(err.retry_after.has_value());
    EXPECT_EQ(err.retry_after->count(), 12);
}

TEST(ErrorClassifier, Timeout408) {
    const auto err = classify_api_error(408, "timeout", {});
    EXPECT_EQ(err.reason, FailoverReason::Timeout);
}

TEST(ErrorClassifier, Context413WithHint) {
    const auto err = classify_api_error(
        413,
        "maximum context length is 32768 tokens",
        {});
    EXPECT_EQ(err.reason, FailoverReason::ContextOverflow);
    ASSERT_TRUE(err.context_limit_hint.has_value());
    EXPECT_EQ(*err.context_limit_hint, 32768);
}

TEST(ErrorClassifier, ContextOverflowFromBody) {
    const auto err = classify_api_error(
        400,
        "error: This model's maximum context length is 200000 tokens",
        {});
    EXPECT_EQ(err.reason, FailoverReason::ContextOverflow);
    ASSERT_TRUE(err.context_limit_hint.has_value());
    EXPECT_EQ(*err.context_limit_hint, 200000);
}

TEST(ErrorClassifier, Unauthorized401) {
    const auto err = classify_api_error(401, "", {});
    EXPECT_EQ(err.reason, FailoverReason::Unauthorized);
}

TEST(ErrorClassifier, Forbidden403) {
    const auto err = classify_api_error(403, "", {});
    EXPECT_EQ(err.reason, FailoverReason::Unauthorized);
}

TEST(ErrorClassifier, ModelUnavailable503) {
    const auto err = classify_api_error(503, "overloaded", {});
    EXPECT_EQ(err.reason, FailoverReason::ModelUnavailable);
}

TEST(ErrorClassifier, ModelNotFoundFromBody) {
    const auto err = classify_api_error(
        400,
        "{\"error\":{\"code\":\"model_not_found\",\"message\":\"no such model\"}}",
        {});
    EXPECT_EQ(err.reason, FailoverReason::ModelUnavailable);
}

TEST(ErrorClassifier, ServerError500) {
    const auto err = classify_api_error(500, "internal error", {});
    EXPECT_EQ(err.reason, FailoverReason::ServerError);
}

TEST(ErrorClassifier, Unknown418) {
    const auto err = classify_api_error(418, "teapot", {});
    EXPECT_EQ(err.reason, FailoverReason::Unknown);
}

TEST(ErrorClassifier, NetworkErrorZeroStatus) {
    const auto err = classify_api_error(0, "connection refused", {});
    EXPECT_EQ(err.reason, FailoverReason::NetworkError);
}
