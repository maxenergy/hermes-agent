// Retry policy helpers: backoff selection + rate-limit header parsing.
#pragma once

#include "hermes/llm/error_classifier.hpp"

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>

namespace hermes::llm {

// Pick the delay before retry `attempt` (1-based) given a classified error.
// Honours Retry-After when present; otherwise falls back to
// hermes::core::retry::jittered_backoff.
std::chrono::milliseconds backoff_for_error(int attempt, const ClassifiedError& err);

// Per-model rate-limit state observed from response headers.
// Recognises the common variants:
//   x-ratelimit-remaining-requests / -tokens
//   x-ratelimit-reset / -reset-requests (seconds or RFC3339)
//   x-ratelimit-reset-after (seconds until reset)
struct RateLimitState {
    std::optional<int64_t> remaining_requests;
    std::optional<int64_t> remaining_tokens;
    std::optional<std::chrono::system_clock::time_point> reset_at;

    void update_from_headers(
        const std::unordered_map<std::string, std::string>& headers);
    bool should_throttle() const;
};

}  // namespace hermes::llm
