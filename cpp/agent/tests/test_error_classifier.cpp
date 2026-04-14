// Tests for hermes::agent::errclass::classify_api_error.
#include "hermes/agent/error_classifier.hpp"

#include <gtest/gtest.h>

using namespace hermes::agent::errclass;

namespace {

ErrorInfo mk(int status, const std::string& msg = "") {
    ErrorInfo e;
    e.status_code = status;
    e.raw_message = msg;
    return e;
}

}  // namespace

TEST(ErrorClassifier, Status401IsAuth) {
    auto c = classify_api_error(mk(401, "Unauthorized"));
    EXPECT_EQ(c.reason, FailoverReason::auth);
    EXPECT_FALSE(c.retryable);
    EXPECT_TRUE(c.should_rotate_credential);
    EXPECT_TRUE(c.is_auth());
}

TEST(ErrorClassifier, Status403KeyLimitIsBilling) {
    auto c = classify_api_error(mk(403, "key limit exceeded"));
    EXPECT_EQ(c.reason, FailoverReason::billing);
    EXPECT_TRUE(c.should_rotate_credential);
}

TEST(ErrorClassifier, Status403PlainIsAuth) {
    auto c = classify_api_error(mk(403, "forbidden"));
    EXPECT_EQ(c.reason, FailoverReason::auth);
}

TEST(ErrorClassifier, Status402TransientIsRateLimit) {
    auto c = classify_api_error(mk(402, "usage limit, try again in 5 minutes"));
    EXPECT_EQ(c.reason, FailoverReason::rate_limit);
    EXPECT_TRUE(c.retryable);
}

TEST(ErrorClassifier, Status402BillingConfirmed) {
    auto c = classify_api_error(mk(402, "insufficient credits"));
    EXPECT_EQ(c.reason, FailoverReason::billing);
    EXPECT_FALSE(c.retryable);
}

TEST(ErrorClassifier, Status404ModelNotFound) {
    auto c = classify_api_error(mk(404, "model not found"));
    EXPECT_EQ(c.reason, FailoverReason::model_not_found);
    EXPECT_TRUE(c.should_fallback);
}

TEST(ErrorClassifier, Status413IsPayloadTooLarge) {
    auto c = classify_api_error(mk(413, ""));
    EXPECT_EQ(c.reason, FailoverReason::payload_too_large);
    EXPECT_TRUE(c.should_compress);
}

TEST(ErrorClassifier, Status429IsRateLimit) {
    auto c = classify_api_error(mk(429, "too many requests"));
    EXPECT_EQ(c.reason, FailoverReason::rate_limit);
    EXPECT_TRUE(c.retryable);
}

TEST(ErrorClassifier, Status429LongContextTier) {
    auto c = classify_api_error(mk(429, "extra usage required for long context tier"));
    EXPECT_EQ(c.reason, FailoverReason::long_context_tier);
    EXPECT_TRUE(c.should_compress);
}

TEST(ErrorClassifier, Status400ThinkingSignature) {
    auto c = classify_api_error(mk(400, "invalid thinking signature in block"));
    EXPECT_EQ(c.reason, FailoverReason::thinking_signature);
    EXPECT_TRUE(c.retryable);
    EXPECT_FALSE(c.should_compress);
}

TEST(ErrorClassifier, Status400ContextOverflow) {
    auto c = classify_api_error(mk(400, "context length exceeded"));
    EXPECT_EQ(c.reason, FailoverReason::context_overflow);
    EXPECT_TRUE(c.should_compress);
}

TEST(ErrorClassifier, Status400LargeSessionGenericMessage) {
    ErrorInfo e = mk(400, "Error");
    e.approx_tokens = 150000;
    e.num_messages = 100;
    auto c = classify_api_error(e);
    EXPECT_EQ(c.reason, FailoverReason::context_overflow);
}

TEST(ErrorClassifier, Status500IsServerError) {
    auto c = classify_api_error(mk(500, "internal"));
    EXPECT_EQ(c.reason, FailoverReason::server_error);
    EXPECT_TRUE(c.retryable);
}

TEST(ErrorClassifier, Status503IsOverloaded) {
    auto c = classify_api_error(mk(503, "unavailable"));
    EXPECT_EQ(c.reason, FailoverReason::overloaded);
}

TEST(ErrorClassifier, ErrorCodeInsufficientQuota) {
    ErrorInfo e;
    e.raw_message = "";
    e.body = nlohmann::json::parse(R"({"error":{"code":"insufficient_quota"}})");
    auto c = classify_api_error(e);
    EXPECT_EQ(c.reason, FailoverReason::billing);
}

TEST(ErrorClassifier, ErrorCodeContextLengthExceeded) {
    ErrorInfo e;
    e.body = nlohmann::json::parse(R"({"error":{"code":"context_length_exceeded"}})");
    auto c = classify_api_error(e);
    EXPECT_EQ(c.reason, FailoverReason::context_overflow);
    EXPECT_TRUE(c.should_compress);
}

TEST(ErrorClassifier, MessageOnlyPayloadTooLarge) {
    ErrorInfo e;
    e.raw_message = "request entity too large";
    auto c = classify_api_error(e);
    EXPECT_EQ(c.reason, FailoverReason::payload_too_large);
}

TEST(ErrorClassifier, MessageOnlyBilling) {
    ErrorInfo e;
    e.raw_message = "credits have been exhausted";
    auto c = classify_api_error(e);
    EXPECT_EQ(c.reason, FailoverReason::billing);
}

TEST(ErrorClassifier, MessageOnlyRateLimit) {
    ErrorInfo e;
    e.raw_message = "too many requests, please retry after 30s";
    auto c = classify_api_error(e);
    EXPECT_EQ(c.reason, FailoverReason::rate_limit);
}

TEST(ErrorClassifier, ServerDisconnectLargeSessionIsContextOverflow) {
    ErrorInfo e;
    e.raw_message = "server disconnected unexpectedly";
    e.approx_tokens = 150000;
    auto c = classify_api_error(e);
    EXPECT_EQ(c.reason, FailoverReason::context_overflow);
}

TEST(ErrorClassifier, ServerDisconnectSmallSessionIsTimeout) {
    ErrorInfo e;
    e.raw_message = "server disconnected";
    e.approx_tokens = 1000;
    auto c = classify_api_error(e);
    EXPECT_EQ(c.reason, FailoverReason::timeout);
}

TEST(ErrorClassifier, TransportErrorTypeIsTimeout) {
    ErrorInfo e;
    e.error_type = "APIConnectionError";
    e.raw_message = "connection reset";
    auto c = classify_api_error(e);
    EXPECT_EQ(c.reason, FailoverReason::timeout);
}

TEST(ErrorClassifier, UnknownFallback) {
    ErrorInfo e;
    e.raw_message = "something weird";
    auto c = classify_api_error(e);
    EXPECT_EQ(c.reason, FailoverReason::unknown);
    EXPECT_TRUE(c.retryable);
}

TEST(ErrorClassifier, MetadataRawParsedForNestedError) {
    ErrorInfo e;
    e.status_code = 400;
    e.raw_message = "Provider returned error";
    e.body = nlohmann::json::parse(
        R"({"error":{"message":"Provider returned error",)"
        R"("metadata":{"raw":"{\"error\":{\"message\":\"context length exceeded\"}}"}}})");
    auto c = classify_api_error(e);
    EXPECT_EQ(c.reason, FailoverReason::context_overflow);
}

TEST(ErrorClassifier, ExtractErrorCodeTopLevel) {
    auto body = nlohmann::json::parse(R"({"code":"throttled"})");
    EXPECT_EQ(detail::extract_error_code(body), "throttled");
}

TEST(ErrorClassifier, ExtractMessageBodyPreferred) {
    auto body = nlohmann::json::parse(R"({"error":{"message":"  hi  "}})");
    EXPECT_EQ(detail::extract_message("raw", body), "hi");
}

TEST(ErrorClassifier, ReasonToStringRoundTrip) {
    EXPECT_EQ(to_string(FailoverReason::rate_limit), "rate_limit");
    EXPECT_EQ(to_string(FailoverReason::context_overflow), "context_overflow");
    EXPECT_EQ(to_string(FailoverReason::unknown), "unknown");
}
