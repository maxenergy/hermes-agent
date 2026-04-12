#include <hermes/gateway/pairing.hpp>

#include <algorithm>
#include <fstream>
#include <random>

namespace hermes::gateway {

namespace {

// 32-char unambiguous alphabet: no 0/O/1/I.
constexpr char kAlphabet[] =
    "23456789ABCDEFGHJKLMNPQRSTUVWXYZ";
constexpr int kAlphabetSize = 32;

}  // namespace

PairingStore::PairingStore(std::filesystem::path pairing_dir)
    : dir_(std::move(pairing_dir)) {
    std::filesystem::create_directories(dir_);
}

std::string PairingStore::generate_code(Platform platform,
                                        const std::string& user_id,
                                        const std::string& user_name) {
    std::lock_guard<std::recursive_mutex> lock(mu_);

    load_pending(platform);

    // Purge expired codes.
    auto now = std::chrono::system_clock::now();
    auto& pending = pending_[platform];
    pending.erase(
        std::remove_if(pending.begin(), pending.end(),
                       [&](const PendingCode& pc) {
                           auto age =
                               std::chrono::duration_cast<
                                   std::chrono::seconds>(now - pc.created_at);
                           return age.count() >= CODE_TTL_SECONDS;
                       }),
        pending.end());

    // Enforce max pending.
    if (static_cast<int>(pending.size()) >= MAX_PENDING) {
        // Remove oldest.
        pending.erase(pending.begin());
    }

    // Generate random code.
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<int> dist(0, kAlphabetSize - 1);

    std::string code;
    code.reserve(CODE_LENGTH);
    for (int i = 0; i < CODE_LENGTH; ++i) {
        code += kAlphabet[dist(gen)];
    }

    pending.push_back({code, user_id, user_name, now});
    save_pending(platform);

    return code;
}

bool PairingStore::is_approved(Platform platform,
                               const std::string& user_id) const {
    std::lock_guard<std::recursive_mutex> lock(mu_);

    // Load from disk if not cached.
    const_cast<PairingStore*>(this)->load_approved(platform);

    auto it = approved_.find(platform);
    if (it == approved_.end()) return false;

    return std::find(it->second.begin(), it->second.end(), user_id) !=
           it->second.end();
}

bool PairingStore::approve_code(Platform platform,
                                const std::string& code) {
    std::lock_guard<std::recursive_mutex> lock(mu_);

    load_pending(platform);

    auto& pending = pending_[platform];
    auto now = std::chrono::system_clock::now();

    for (auto it = pending.begin(); it != pending.end(); ++it) {
        if (it->code == code) {
            // Check expiry.
            auto age = std::chrono::duration_cast<std::chrono::seconds>(
                now - it->created_at);
            if (age.count() >= CODE_TTL_SECONDS) {
                pending.erase(it);
                save_pending(platform);
                return false;
            }

            // Approve the user.
            std::string uid = it->user_id;
            pending.erase(it);
            save_pending(platform);

            load_approved(platform);
            auto& approved = approved_[platform];
            if (std::find(approved.begin(), approved.end(), uid) ==
                approved.end()) {
                approved.push_back(uid);
            }
            save_approved(platform);
            return true;
        }
    }

    return false;
}

bool PairingStore::is_rate_limited(Platform platform,
                                   const std::string& user_id) const {
    std::lock_guard<std::recursive_mutex> lock(mu_);

    auto key = make_rate_key(platform, user_id);
    auto it = rate_limits_.find(key);
    if (it == rate_limits_.end()) return false;

    auto now = std::chrono::system_clock::now();

    // Check lockout.
    if (it->second.failure_count >= 5) {
        if (now < it->second.lockout_until) {
            return true;
        }
        // Lockout expired — reset.
        it->second.failure_count = 0;
    }

    // Check per-request rate: 1 per 10 min.
    auto elapsed = std::chrono::duration_cast<std::chrono::minutes>(
        now - it->second.last_request);
    return elapsed.count() < 10;
}

std::string PairingStore::make_rate_key(
    Platform platform, const std::string& user_id) const {
    return platform_to_string(platform) + ":" + user_id;
}

// --- Persistence helpers ---

void PairingStore::load_approved(Platform platform) {
    auto path =
        dir_ / (platform_to_string(platform) + "-approved.json");
    if (!std::filesystem::exists(path)) return;

    // Skip if already loaded.
    if (approved_.count(platform) > 0) return;

    std::ifstream in(path);
    nlohmann::json j;
    in >> j;
    auto& vec = approved_[platform];
    vec.clear();
    for (auto& uid : j) {
        vec.push_back(uid.get<std::string>());
    }
}

void PairingStore::save_approved(Platform platform) const {
    auto path =
        dir_ / (platform_to_string(platform) + "-approved.json");
    nlohmann::json j = nlohmann::json::array();
    auto it = approved_.find(platform);
    if (it != approved_.end()) {
        for (auto& uid : it->second) {
            j.push_back(uid);
        }
    }
    std::ofstream out(path);
    out << j.dump(2);
}

void PairingStore::load_pending(Platform platform) {
    auto path =
        dir_ / (platform_to_string(platform) + "-pending.json");
    if (!std::filesystem::exists(path)) return;

    std::ifstream in(path);
    nlohmann::json j;
    in >> j;
    auto& vec = pending_[platform];
    vec.clear();
    for (auto& item : j) {
        PendingCode pc;
        pc.code = item.value("code", "");
        pc.user_id = item.value("user_id", "");
        pc.user_name = item.value("user_name", "");
        // Parse created_at as epoch seconds.
        pc.created_at = std::chrono::system_clock::time_point(
            std::chrono::seconds(item.value("created_at", 0LL)));
        vec.push_back(std::move(pc));
    }
}

void PairingStore::save_pending(Platform platform) const {
    auto path =
        dir_ / (platform_to_string(platform) + "-pending.json");
    nlohmann::json j = nlohmann::json::array();
    auto it = pending_.find(platform);
    if (it != pending_.end()) {
        for (auto& pc : it->second) {
            j.push_back(nlohmann::json{
                {"code", pc.code},
                {"user_id", pc.user_id},
                {"user_name", pc.user_name},
                {"created_at",
                 std::chrono::duration_cast<std::chrono::seconds>(
                     pc.created_at.time_since_epoch())
                     .count()},
            });
        }
    }
    std::ofstream out(path);
    out << j.dump(2);
}

}  // namespace hermes::gateway
