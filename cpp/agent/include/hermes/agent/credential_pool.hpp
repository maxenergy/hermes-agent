// Credential-pool types & strategy helpers.
//
// Partial C++17 port of agent/credential_pool.py. Models the immutable
// "entry" record (API key or OAuth) with its exhaustion TTL, and the
// pick-next strategy selection logic. Actual I/O (reading/writing
// ~/.hermes/auth.json) is deferred to the hermes_state / hermes_cli
// auth layer.
#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace hermes::agent::creds {

// Status / auth-type / source / strategy constants mirroring Python.
namespace status {
constexpr const char* kOk = "ok";
constexpr const char* kExhausted = "exhausted";
}  // namespace status

namespace auth_type {
constexpr const char* kOAuth = "oauth";
constexpr const char* kApiKey = "api_key";
}  // namespace auth_type

namespace source {
constexpr const char* kManual = "manual";
}  // namespace source

enum class Strategy {
    FillFirst,
    RoundRobin,
    Random,
    LeastUsed,
};

std::string strategy_name(Strategy s);
// Returns std::nullopt when the name is not a valid strategy.
std::optional<Strategy> parse_strategy(const std::string& name);

// Cooldown before retrying an exhausted credential. Both 429 and 402
// cool down after 1 hour by default (provider reset_at overrides).
constexpr std::int64_t kExhaustedTtl429Seconds = 60 * 60;
constexpr std::int64_t kExhaustedTtlDefaultSeconds = 60 * 60;

// Custom endpoint pool key prefix (Python: CUSTOM_POOL_PREFIX).
constexpr const char* kCustomPoolPrefix = "custom:";

struct PoolEntry {
    std::string id;
    std::string auth_type = auth_type::kApiKey;
    std::string source = source::kManual;
    std::string api_key;             // empty for OAuth entries
    std::string refresh_token;
    std::string access_token;
    std::int64_t expires_at_sec = 0; // 0 = never
    std::string status = status::kOk;
    std::int64_t exhausted_until_sec = 0;
    std::int64_t last_used_sec = 0;
    std::int64_t use_count = 0;
    std::string provider;
    std::string base_url;
    std::string label;               // user-facing alias
};

// Return the subset of `entries` whose status is OK (not exhausted,
// or exhausted_until has already passed `now_sec`).
std::vector<PoolEntry> filter_available(const std::vector<PoolEntry>& entries,
                                        std::int64_t now_sec);

// Choose the next credential given strategy + availability. Returns
// std::nullopt when no entries are available.
std::optional<PoolEntry> choose_next(const std::vector<PoolEntry>& entries,
                                     Strategy strategy,
                                     std::int64_t now_sec,
                                     std::uint64_t round_robin_cursor = 0,
                                     std::uint64_t random_seed = 0);

// Mark an entry exhausted, returning a copy with status/until set.
// `until_sec = 0` → use kExhaustedTtlDefaultSeconds from now_sec.
PoolEntry mark_exhausted(PoolEntry entry,
                         std::int64_t now_sec,
                         std::int64_t until_sec = 0);

// Return the pool key for a provider + optional custom-endpoint name.
std::string pool_key(const std::string& provider,
                     const std::string& custom_name = "");

}  // namespace hermes::agent::creds
