// Thread-safe credential cache with TTL + refresh hook.
//
// Mirrors the Python `agent/credential_pool.py` + `PooledCredential`
// surface in the minimum shape that runtime_provider and LlmClient
// subclasses need:  a per-provider cache of (api_key, base_url,
// expires_at) that can evict expired entries and invoke a refresh
// callback to repopulate them on miss.
//
// Supports multiple credentials per provider with round-robin selection
// (mirrors the Python pool).  Single-slot callers keep working via
// `store()`, which replaces the provider's list with exactly one entry.
#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace hermes::llm {

struct PooledCredential {
    std::string api_key;
    std::string base_url;
    // Absolute wall-clock expiry.  A default-constructed time_point
    // (epoch) means "never expires".
    std::chrono::system_clock::time_point expires_at{};
    // Free-form provenance string (e.g. "env", "oauth", "portal").
    std::string source;

    // Returns true when expires_at is set and already elapsed.
    bool is_expired(std::chrono::system_clock::time_point now =
                        std::chrono::system_clock::now()) const;
    // True if api_key is non-empty and not expired.
    bool is_usable(std::chrono::system_clock::time_point now =
                       std::chrono::system_clock::now()) const;
};

class CredentialPool {
public:
    // A refresher is called when a provider is looked up and either
    // (a) no entry exists, or (b) the cached entry is expired.  It
    // should return a fresh credential, or std::nullopt to signal
    // "no credentials available" (the cache entry will be cleared).
    using Refresher = std::function<std::optional<PooledCredential>(
        const std::string& provider)>;

    CredentialPool() = default;

    // Register (or replace) a refresher for a provider.  Thread-safe.
    void set_refresher(const std::string& provider, Refresher fn);

    // Cache a credential under `provider` directly, bypassing any
    // refresher.  Useful for tests and for providers whose credentials
    // are supplied explicitly (e.g. via config.yaml).  Replaces any
    // existing credential(s) for the provider with a single entry.
    void store(const std::string& provider, PooledCredential cred);

    // Append a credential to the provider's round-robin list without
    // dropping previously stored entries.  Duplicates by api_key are
    // skipped.
    void add(const std::string& provider, PooledCredential cred);

    // Return the number of credentials currently stored for `provider`.
    std::size_t count(const std::string& provider) const;

    // Look up a credential.  If the cached entry is expired and a
    // refresher is registered, the refresher is called and its result
    // replaces the slot.  Returns std::nullopt when no usable entry
    // can be produced.
    std::optional<PooledCredential> get(const std::string& provider);

    // Drop a single provider's entry.
    void invalidate(const std::string& provider);

    // Drop every cached entry (refreshers are preserved).
    void clear();

    // Evict every expired entry.  Returns the count of evictions.
    std::size_t evict_expired(
        std::chrono::system_clock::time_point now =
            std::chrono::system_clock::now());

    // Number of cached entries (including expired-but-not-yet-evicted).
    std::size_t size() const;

    // Process-wide singleton used by runtime_provider.  Separate tests
    // create their own CredentialPool instances; production code goes
    // through this accessor.
    static CredentialPool& global();

private:
    mutable std::mutex mu_;
    // Round-robin slot list per provider.  Cursor tracks the next
    // credential to return from `get()`.
    struct Slot {
        std::vector<PooledCredential> creds;
        std::uint64_t cursor = 0;
    };
    std::unordered_map<std::string, Slot> slots_;
    std::unordered_map<std::string, Refresher> refreshers_;
};

}  // namespace hermes::llm
