// API error classifier producing structured failover reasons with optional
// retry_after / context_limit hints.  Simplified port of
// agent/error_classifier.py — only the reasons Phase 3 actually drives on.
#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

namespace hermes::llm {

enum class FailoverReason {
    None,
    RateLimit,         // 429
    ContextOverflow,   // 413, or body names context length
    Unauthorized,      // 401 / 403
    ModelUnavailable,  // 503 / model_not_found
    ServerError,       // generic 5xx
    Timeout,           // 408
    NetworkError,      // transport failure (no response)
    Unknown,
};

struct ClassifiedError {
    FailoverReason reason = FailoverReason::Unknown;
    std::string detail;
    std::optional<std::chrono::seconds> retry_after;
    std::optional<int64_t> context_limit_hint;
};

ClassifiedError classify_api_error(
    int status_code,
    std::string_view body,
    const std::unordered_map<std::string, std::string>& headers = {});

}  // namespace hermes::llm
