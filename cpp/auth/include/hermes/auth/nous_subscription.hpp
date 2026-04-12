// Nous Research subscription status check.
#pragma once

#include <chrono>
#include <optional>
#include <string>
#include <vector>

namespace hermes::llm {
class HttpTransport;
}

namespace hermes::auth {

struct SubscriptionStatus {
    bool active = false;
    std::string tier;   // "free" | "pro" | "enterprise"
    std::chrono::system_clock::time_point expires_at{};
    std::vector<std::string> features;
    std::string user_id;
};

// GET {base_url}/v1/subscription with Bearer api_key.
// Returns nullopt on HTTP error.
std::optional<SubscriptionStatus> check_subscription(
    const std::string& api_key,
    hermes::llm::HttpTransport* transport = nullptr,
    const std::string& base_url = "https://api.nousresearch.com");

// Cached version — refreshes every hour to avoid spamming.
// Uses the currently-configured NOUS_API_KEY env var.
std::optional<SubscriptionStatus> cached_subscription_status();

// Invalidate the cache (used by tests and on logout).
void clear_subscription_cache();

}  // namespace hermes::auth
