// Rate limit tracking for inference API responses.
//
// C++17 port of agent/rate_limit_tracker.py. Captures x-ratelimit-* headers
// from provider responses and provides formatted display for the /usage
// slash command. Follows the Nous Portal / OpenRouter header schema (12
// headers total: limit/remaining/reset x requests/tokens x min/1h).
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>

namespace hermes::agent {

struct RateLimitBucket {
    std::int64_t limit = 0;
    std::int64_t remaining = 0;
    double reset_seconds = 0.0;
    double captured_at = 0.0;  // seconds since epoch when captured

    std::int64_t used() const noexcept;
    double usage_pct() const noexcept;
    double remaining_seconds_now(double now = 0.0) const noexcept;
};

struct RateLimitState {
    RateLimitBucket requests_min;
    RateLimitBucket requests_hour;
    RateLimitBucket tokens_min;
    RateLimitBucket tokens_hour;
    double captured_at = 0.0;
    std::string provider;

    bool has_data() const noexcept { return captured_at > 0.0; }
    double age_seconds(double now = 0.0) const noexcept;
};

// Parse x-ratelimit-* headers (case-insensitive). Returns std::nullopt when
// no rate limit headers are present in the map.
std::optional<RateLimitState> parse_rate_limit_headers(
    const std::unordered_map<std::string, std::string>& headers,
    const std::string& provider = "");

// Multi-line formatted display for terminal/chat.
std::string format_rate_limit_display(const RateLimitState& state,
                                      double now = 0.0);

// One-line compact summary for status bars.
std::string format_rate_limit_compact(const RateLimitState& state,
                                      double now = 0.0);

// Internals exposed for tests.
namespace detail {
std::string fmt_count(std::int64_t n);
std::string fmt_seconds(double seconds);
std::string bar(double pct, int width = 20);
}  // namespace detail

}  // namespace hermes::agent
