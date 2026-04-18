// Retry-backoff helpers.  Implements per-FailoverReason tier map mirroring
// run_agent.py::_compute_next_backoff and agent/retry_utils.py::jittered_backoff:
//
//     RateLimit    → base=3s   cap=60s   factor=1.8   (Retry-After wins)
//     ServerError  → base=1.5s cap=30s   factor=2.0
//     NetworkError → base=1s   cap=15s   factor=2.0
//     Timeout      → base=2s   cap=20s   factor=1.5
//     <other>      → delegates to hermes::core::retry::jittered_backoff
//
// All tiered branches apply ±20% jitter via a thread-local std::mt19937.  The
// public API `backoff_for_error(attempt, ClassifiedError)` is unchanged so
// every call-site in llm/ keeps working without edits.
#include "hermes/llm/retry_policy.hpp"

#include "hermes/core/retry.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <ctime>
#include <random>
#include <string>

namespace hermes::llm {

namespace {

std::string to_lower(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        out.push_back(static_cast<char>(
            std::tolower(static_cast<unsigned char>(c))));
    }
    return out;
}

std::optional<int64_t> parse_int(const std::string& s) {
    if (s.empty()) return std::nullopt;
    int64_t v = 0;
    auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), v);
    if (ec != std::errc()) return std::nullopt;
    return v;
}

std::mt19937& tier_rng() {
    thread_local std::mt19937 rng{std::random_device{}()};
    return rng;
}

struct BackoffTier {
    double base_seconds;
    double cap_seconds;
    double factor;
};

std::optional<BackoffTier> tier_for(FailoverReason reason) {
    switch (reason) {
        case FailoverReason::RateLimit:
            return BackoffTier{3.0, 60.0, 1.8};
        case FailoverReason::ServerError:
        case FailoverReason::ModelUnavailable:
            // 5xx family — includes 503/model-unavailable which the Python
            // loop lumps into the same tier as generic server errors.
            return BackoffTier{1.5, 30.0, 2.0};
        case FailoverReason::NetworkError:
            return BackoffTier{1.0, 15.0, 2.0};
        case FailoverReason::Timeout:
            return BackoffTier{2.0, 20.0, 1.5};
        default:
            return std::nullopt;
    }
}

/// Compute tiered backoff: `min(base * factor^(attempt-1), cap)` with
/// ±20% uniform jitter.  Always returns a non-negative duration.
std::chrono::milliseconds tiered_backoff(int attempt, const BackoffTier& tier) {
    if (attempt < 1) attempt = 1;
    // Cap the exponent so pow() can't blow up on pathological attempt counts.
    const int exponent = std::min(attempt - 1, 20);
    double delay = tier.base_seconds * std::pow(tier.factor,
                                                static_cast<double>(exponent));
    if (!std::isfinite(delay) || delay < 0.0) delay = tier.cap_seconds;
    if (delay > tier.cap_seconds) delay = tier.cap_seconds;

    // ±20% jitter — matches the jitter_ratio 0.4 spread split around the
    // centre (so the effective range is [0.8*delay, 1.2*delay]).
    std::uniform_real_distribution<double> dist(-0.2, 0.2);
    const double jittered = delay * (1.0 + dist(tier_rng()));
    const double ms = jittered * 1000.0;
    if (!std::isfinite(ms) || ms < 0.0) {
        return std::chrono::milliseconds(0);
    }
    return std::chrono::milliseconds(static_cast<int64_t>(ms));
}

}  // namespace

std::chrono::milliseconds backoff_for_error(int attempt, const ClassifiedError& err) {
    // Retry-After always wins for RateLimit — matches Python's behaviour of
    // honouring the server hint before running its own curve.
    if (err.retry_after) {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            *err.retry_after);
    }
    if (auto tier = tier_for(err.reason)) {
        return tiered_backoff(attempt, *tier);
    }
    return hermes::core::retry::jittered_backoff(attempt);
}

void RateLimitState::update_from_headers(
    const std::unordered_map<std::string, std::string>& headers) {
    for (const auto& kv : headers) {
        const std::string key = to_lower(kv.first);
        if (key == "x-ratelimit-remaining-requests") {
            remaining_requests = parse_int(kv.second);
        } else if (key == "x-ratelimit-remaining-tokens") {
            remaining_tokens = parse_int(kv.second);
        } else if (key == "x-ratelimit-reset" ||
                   key == "x-ratelimit-reset-requests" ||
                   key == "x-ratelimit-reset-tokens") {
            // Heuristic: if the value looks like a plain number, treat
            // it as seconds-since-epoch.  Everything else is ignored
            // (OpenAI's "1m30s" parsing lives elsewhere).
            if (auto v = parse_int(kv.second); v.has_value()) {
                using namespace std::chrono;
                // Treat small values (<10^9) as seconds-until-reset.
                if (*v < 1'000'000'000) {
                    reset_at = system_clock::now() + seconds(*v);
                } else {
                    reset_at = system_clock::time_point(seconds(*v));
                }
            }
        } else if (key == "x-ratelimit-reset-after") {
            if (auto v = parse_int(kv.second); v.has_value()) {
                using namespace std::chrono;
                reset_at = system_clock::now() + seconds(*v);
            }
        }
    }
}

bool RateLimitState::should_throttle() const {
    if (remaining_requests && *remaining_requests <= 0) return true;
    if (remaining_tokens && *remaining_tokens <= 0) return true;
    return false;
}

}  // namespace hermes::llm
