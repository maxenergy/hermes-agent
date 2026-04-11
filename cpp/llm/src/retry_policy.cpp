// Retry-backoff helpers.  Delegates to hermes::core::retry::jittered_backoff
// and overlays provider-supplied Retry-After hints when available.
#include "hermes/llm/retry_policy.hpp"

#include "hermes/core/retry.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <ctime>
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

}  // namespace

std::chrono::milliseconds backoff_for_error(int attempt, const ClassifiedError& err) {
    if (err.retry_after) {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            *err.retry_after);
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
