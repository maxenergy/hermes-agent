// API error classification for smart failover and recovery.
//
// C++17 port of agent/error_classifier.py. Provides a structured taxonomy
// of API errors and a priority-ordered pipeline that determines the
// correct recovery action (retry, rotate credential, fallback, compress,
// or abort).
//
// Unlike the Python version, which walks Exception.__cause__ chains and
// pulls .status_code / .body off SDK objects, this API takes a pre-built
// ErrorInfo struct: the caller (LLM adapter) extracts status code and
// body at the source and hands them to classify_api_error.
#pragma once

#include <nlohmann/json.hpp>

#include <optional>
#include <string>

namespace hermes::agent::errclass {

enum class FailoverReason {
    auth,
    auth_permanent,
    billing,
    rate_limit,
    overloaded,
    server_error,
    timeout,
    context_overflow,
    payload_too_large,
    model_not_found,
    format_error,
    thinking_signature,
    long_context_tier,
    unknown,
};

std::string to_string(FailoverReason r);

struct ClassifiedError {
    FailoverReason reason = FailoverReason::unknown;
    std::optional<int> status_code;
    std::string provider;
    std::string model;
    std::string message;

    bool retryable = true;
    bool should_compress = false;
    bool should_rotate_credential = false;
    bool should_fallback = false;

    bool is_auth() const noexcept {
        return reason == FailoverReason::auth ||
               reason == FailoverReason::auth_permanent;
    }
};

// Structured input from an SDK exception. Callers are expected to fill
// whichever fields they have; missing fields fall through to message /
// body pattern matching.
struct ErrorInfo {
    std::optional<int> status_code;
    std::string error_type;              // e.g. "APIConnectionError"
    std::string raw_message;             // str(error)
    nlohmann::json body = nlohmann::json::object();
    std::string provider;
    std::string model;
    int approx_tokens = 0;
    int context_length = 200000;
    int num_messages = 0;
};

// Main entry point: priority-ordered classification pipeline.
ClassifiedError classify_api_error(const ErrorInfo& info);

// Helpers exposed for tests.
namespace detail {
std::string extract_error_code(const nlohmann::json& body);
std::string extract_message(const std::string& raw, const nlohmann::json& body);
std::string build_error_msg(const std::string& raw, const nlohmann::json& body);
bool any_of_patterns(const std::string& lowered_text,
                     const std::initializer_list<const char*>& patterns);
}  // namespace detail

}  // namespace hermes::agent::errclass
