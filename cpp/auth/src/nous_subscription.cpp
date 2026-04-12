#include "hermes/auth/nous_subscription.hpp"

#include <nlohmann/json.hpp>

#include <cstdlib>
#include <ctime>
#include <mutex>

#include "hermes/llm/llm_client.hpp"

namespace hermes::auth {
namespace {

struct Cache {
    std::optional<SubscriptionStatus> value;
    std::chrono::steady_clock::time_point fetched_at;
};

std::mutex g_cache_mu;
Cache g_cache;
constexpr auto kCacheTtl = std::chrono::hours(1);

std::chrono::system_clock::time_point parse_iso8601(const std::string& s) {
    // Minimal ISO 8601 parser — handles YYYY-MM-DDTHH:MM:SSZ
    std::tm tm{};
    if (s.size() < 19) return {};
    tm.tm_year = std::atoi(s.substr(0, 4).c_str()) - 1900;
    tm.tm_mon  = std::atoi(s.substr(5, 2).c_str()) - 1;
    tm.tm_mday = std::atoi(s.substr(8, 2).c_str());
    tm.tm_hour = std::atoi(s.substr(11, 2).c_str());
    tm.tm_min  = std::atoi(s.substr(14, 2).c_str());
    tm.tm_sec  = std::atoi(s.substr(17, 2).c_str());
    // timegm is GNU; use mktime and adjust — or portable: treat as UTC.
    auto tt = timegm(&tm);
    return std::chrono::system_clock::from_time_t(tt);
}

}  // namespace

std::optional<SubscriptionStatus> check_subscription(
    const std::string& api_key,
    hermes::llm::HttpTransport* transport,
    const std::string& base_url) {
    if (!transport) transport = hermes::llm::get_default_transport();
    if (!transport || api_key.empty()) return std::nullopt;

    auto resp = transport->get(
        base_url + "/v1/subscription",
        {{"Authorization", "Bearer " + api_key},
         {"Accept", "application/json"}});
    if (resp.status_code < 200 || resp.status_code >= 300) return std::nullopt;

    auto j = nlohmann::json::parse(resp.body, nullptr, false);
    if (j.is_discarded()) return std::nullopt;

    SubscriptionStatus s;
    s.active = j.value("active", false);
    s.tier = j.value("tier", "free");
    s.user_id = j.value("user_id", "");
    if (j.contains("expires_at") && j["expires_at"].is_string()) {
        s.expires_at = parse_iso8601(j["expires_at"].get<std::string>());
    }
    if (j.contains("features") && j["features"].is_array()) {
        for (const auto& f : j["features"]) {
            if (f.is_string()) s.features.push_back(f.get<std::string>());
        }
    }
    return s;
}

std::optional<SubscriptionStatus> cached_subscription_status() {
    std::lock_guard<std::mutex> lock(g_cache_mu);
    auto now = std::chrono::steady_clock::now();
    if (g_cache.value && (now - g_cache.fetched_at) < kCacheTtl) {
        return g_cache.value;
    }

    const char* key = std::getenv("NOUS_API_KEY");
    if (!key) return std::nullopt;

    auto result = check_subscription(key);
    g_cache.value = result;
    g_cache.fetched_at = now;
    return result;
}

void clear_subscription_cache() {
    std::lock_guard<std::mutex> lock(g_cache_mu);
    g_cache.value.reset();
}

}  // namespace hermes::auth
