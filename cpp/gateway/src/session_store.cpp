#include <hermes/gateway/session_store.hpp>

#include <algorithm>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <sstream>

#include <openssl/sha.h>

namespace hermes::gateway {

namespace {

std::string time_to_iso(std::chrono::system_clock::time_point tp) {
    auto t = std::chrono::system_clock::to_time_t(tp);
    std::tm tm{};
    gmtime_r(&t, &tm);
    std::ostringstream oss;
    oss << std::put_time(&tm, "%Y-%m-%dT%H:%M:%SZ");
    return oss.str();
}

std::chrono::system_clock::time_point iso_to_time(const std::string& s) {
    std::tm tm{};
    std::istringstream iss(s);
    iss >> std::get_time(&tm, "%Y-%m-%dT%H:%M:%SZ");
    return std::chrono::system_clock::from_time_t(timegm(&tm));
}

// Build a deterministic session key from the source.
std::string make_session_key(const SessionSource& src) {
    return platform_to_string(src.platform) + ":" + src.chat_id + ":" +
           src.user_id;
}

bool should_redact_platform(Platform p) {
    return p == Platform::Telegram || p == Platform::Signal ||
           p == Platform::WhatsApp || p == Platform::BlueBubbles;
}

}  // namespace

// --- SessionSource ---

std::string SessionSource::description() const {
    std::string desc = platform_to_string(platform) + "/" + chat_type;
    if (!chat_name.empty())
        desc += " (" + chat_name + ")";
    if (!user_name.empty())
        desc += " user=" + user_name;
    return desc;
}

nlohmann::json SessionSource::to_json() const {
    return nlohmann::json{
        {"platform", platform_to_string(platform)},
        {"chat_id", chat_id},
        {"chat_name", chat_name},
        {"chat_type", chat_type},
        {"user_id", user_id},
        {"user_name", user_name},
        {"thread_id", thread_id},
        {"chat_topic", chat_topic},
        {"user_id_alt", user_id_alt},
        {"chat_id_alt", chat_id_alt},
    };
}

SessionSource SessionSource::from_json(const nlohmann::json& j) {
    SessionSource src;
    src.platform = platform_from_string(j.value("platform", "local"));
    src.chat_id = j.value("chat_id", "");
    src.chat_name = j.value("chat_name", "");
    src.chat_type = j.value("chat_type", "dm");
    src.user_id = j.value("user_id", "");
    src.user_name = j.value("user_name", "");
    src.thread_id = j.value("thread_id", "");
    src.chat_topic = j.value("chat_topic", "");
    src.user_id_alt = j.value("user_id_alt", "");
    src.chat_id_alt = j.value("chat_id_alt", "");
    return src;
}

// --- SessionContext ---

nlohmann::json SessionContext::to_json() const {
    nlohmann::json j;
    j["source"] = source.to_json();
    j["session_key"] = session_key;
    j["session_id"] = session_id;
    j["created_at"] = time_to_iso(created_at);
    j["updated_at"] = time_to_iso(updated_at);

    nlohmann::json plats = nlohmann::json::array();
    for (auto p : connected_platforms) {
        plats.push_back(platform_to_string(p));
    }
    j["connected_platforms"] = plats;

    nlohmann::json hc_map = nlohmann::json::object();
    for (auto& [p, ch] : home_channels) {
        hc_map[platform_to_string(p)] = nlohmann::json{
            {"chat_id", ch.chat_id},
            {"name", ch.name},
        };
    }
    j["home_channels"] = hc_map;
    return j;
}

// --- SessionStore ---

SessionStore::SessionStore(std::filesystem::path sessions_dir)
    : dir_(std::move(sessions_dir)) {
    std::filesystem::create_directories(dir_);
}

std::string SessionStore::get_or_create_session(
    const SessionSource& source) {
    std::lock_guard<std::recursive_mutex> lock(mu_);

    std::string key = make_session_key(source);
    auto session_dir = dir_ / key;
    auto session_file = session_dir / "session.json";

    if (std::filesystem::exists(session_file)) {
        // Update the updated_at timestamp.
        std::ifstream in(session_file);
        nlohmann::json j;
        in >> j;
        in.close();

        j["updated_at"] = time_to_iso(std::chrono::system_clock::now());
        std::ofstream out(session_file);
        out << j.dump(2);
        return key;
    }

    // Create new session.
    std::filesystem::create_directories(session_dir);

    auto now = std::chrono::system_clock::now();
    SessionContext ctx;
    ctx.source = source;
    ctx.session_key = key;
    ctx.session_id = key;  // Simplified; a UUID would be better.
    ctx.created_at = now;
    ctx.updated_at = now;

    std::ofstream out(session_file);
    out << ctx.to_json().dump(2);
    return key;
}

void SessionStore::reset_session(const std::string& session_key) {
    std::lock_guard<std::recursive_mutex> lock(mu_);

    auto session_dir = dir_ / session_key;
    auto transcript = session_dir / "transcript.jsonl";

    // Remove transcript to reset.
    if (std::filesystem::exists(transcript)) {
        std::filesystem::remove(transcript);
    }

    // Update timestamp in session.json.
    auto session_file = session_dir / "session.json";
    if (std::filesystem::exists(session_file)) {
        std::ifstream in(session_file);
        nlohmann::json j;
        in >> j;
        in.close();

        j["updated_at"] = time_to_iso(std::chrono::system_clock::now());
        std::ofstream out(session_file);
        out << j.dump(2);
    }
}

void SessionStore::append_message(const std::string& session_key,
                                  const nlohmann::json& message) {
    std::lock_guard<std::recursive_mutex> lock(mu_);

    auto session_dir = dir_ / session_key;
    std::filesystem::create_directories(session_dir);

    auto transcript = session_dir / "transcript.jsonl";
    std::ofstream out(transcript, std::ios::app);
    out << message.dump() << "\n";
}

std::vector<nlohmann::json> SessionStore::load_transcript(
    const std::string& session_key) {
    std::lock_guard<std::recursive_mutex> lock(mu_);

    std::vector<nlohmann::json> messages;
    auto transcript = dir_ / session_key / "transcript.jsonl";

    if (!std::filesystem::exists(transcript)) {
        return messages;
    }

    std::ifstream in(transcript);
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty()) {
            messages.push_back(nlohmann::json::parse(line));
        }
    }
    return messages;
}

bool SessionStore::should_reset(const std::string& session_key,
                                const SessionResetPolicy& policy) {
    std::lock_guard<std::recursive_mutex> lock(mu_);

    if (policy.mode == "none") {
        return false;
    }

    auto session_file = dir_ / session_key / "session.json";
    if (!std::filesystem::exists(session_file)) {
        return false;
    }

    std::ifstream in(session_file);
    nlohmann::json j;
    in >> j;

    auto updated = iso_to_time(j.value("updated_at", ""));
    auto now = std::chrono::system_clock::now();

    bool idle_trigger = false;
    bool daily_trigger = false;

    if (policy.mode == "idle" || policy.mode == "both") {
        auto elapsed =
            std::chrono::duration_cast<std::chrono::minutes>(now - updated);
        idle_trigger = elapsed.count() >= policy.idle_minutes;
    }

    if (policy.mode == "daily" || policy.mode == "both") {
        // Check if we've crossed the reset hour since last update.
        auto now_t = std::chrono::system_clock::to_time_t(now);
        auto upd_t = std::chrono::system_clock::to_time_t(updated);
        std::tm now_tm{}, upd_tm{};
        gmtime_r(&now_t, &now_tm);
        gmtime_r(&upd_t, &upd_tm);

        // Different day and current hour >= at_hour.
        if (now_tm.tm_yday != upd_tm.tm_yday ||
            now_tm.tm_year != upd_tm.tm_year) {
            if (now_tm.tm_hour >= policy.at_hour) {
                daily_trigger = true;
            }
        }
    }

    if (policy.mode == "both") {
        return idle_trigger || daily_trigger;
    }
    if (policy.mode == "idle") {
        return idle_trigger;
    }
    if (policy.mode == "daily") {
        return daily_trigger;
    }

    return false;
}

std::string SessionStore::hash_id(std::string_view id) {
    unsigned char hash[SHA256_DIGEST_LENGTH];
    SHA256(reinterpret_cast<const unsigned char*>(id.data()), id.size(),
           hash);

    std::ostringstream oss;
    for (int i = 0; i < 6; ++i) {  // 6 bytes = 12 hex chars
        oss << std::hex << std::setfill('0') << std::setw(2)
            << static_cast<int>(hash[i]);
    }
    return oss.str();
}

SessionSource SessionStore::redact_pii(const SessionSource& source) const {
    SessionSource redacted = source;

    if (should_redact_platform(source.platform)) {
        if (!redacted.user_id.empty())
            redacted.user_id = hash_id(redacted.user_id);
        if (!redacted.chat_id.empty())
            redacted.chat_id = hash_id(redacted.chat_id);
        if (!redacted.user_id_alt.empty())
            redacted.user_id_alt = hash_id(redacted.user_id_alt);
        if (!redacted.chat_id_alt.empty())
            redacted.chat_id_alt = hash_id(redacted.chat_id_alt);
        // Redact names too.
        if (!redacted.user_name.empty())
            redacted.user_name = "redacted";
        if (!redacted.chat_name.empty())
            redacted.chat_name = "redacted";
    }

    return redacted;
}

}  // namespace hermes::gateway
