// DM authorization / pairing store.
#pragma once

#include <chrono>
#include <filesystem>
#include <map>
#include <mutex>
#include <string>

#include <nlohmann/json.hpp>

#include <hermes/gateway/gateway_config.hpp>

namespace hermes::gateway {

class PairingStore {
public:
    explicit PairingStore(std::filesystem::path pairing_dir);

    // Generate 8-char code from 32-char unambiguous alphabet (no 0/O/1/I)
    std::string generate_code(Platform platform,
                              const std::string& user_id,
                              const std::string& user_name);
    bool is_approved(Platform platform, const std::string& user_id) const;
    bool approve_code(Platform platform, const std::string& code);

    // Rate limiting: 1 req per user per 10 min, lockout after 5 failures
    // for 1 hour
    bool is_rate_limited(Platform platform,
                         const std::string& user_id) const;

    static constexpr int MAX_PENDING = 3;
    static constexpr int CODE_LENGTH = 8;
    static constexpr int CODE_TTL_SECONDS = 3600;

private:
    std::filesystem::path dir_;
    mutable std::recursive_mutex mu_;

    // In-memory pending codes: platform -> [{code, user_id, user_name,
    // created_at}]
    struct PendingCode {
        std::string code;
        std::string user_id;
        std::string user_name;
        std::chrono::system_clock::time_point created_at;
    };
    std::map<Platform, std::vector<PendingCode>> pending_;

    // Approved users: platform -> set of user_ids
    std::map<Platform, std::vector<std::string>> approved_;

    // Rate limiting state
    struct RateLimitEntry {
        std::chrono::system_clock::time_point last_request;
        int failure_count = 0;
        std::chrono::system_clock::time_point lockout_until;
    };
    mutable std::map<std::string, RateLimitEntry> rate_limits_;

    std::string make_rate_key(Platform platform,
                              const std::string& user_id) const;

    // Persistence helpers
    void load_approved(Platform platform);
    void save_approved(Platform platform) const;
    void load_pending(Platform platform);
    void save_pending(Platform platform) const;
};

}  // namespace hermes::gateway
