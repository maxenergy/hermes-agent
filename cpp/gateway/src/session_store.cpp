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

    // Resume/suspend state (upstream cb4addac).
    j["suspended"] = suspended;
    j["resume_pending"] = resume_pending;
    if (!resume_reason.empty()) j["resume_reason"] = resume_reason;
    if (last_resume_marked_at.time_since_epoch().count() > 0) {
        j["last_resume_marked_at"] = time_to_iso(last_resume_marked_at);
    }

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
        std::ifstream in(session_file);
        nlohmann::json j;
        in >> j;
        in.close();

        // Hard forced-wipe takes priority over resume_pending (upstream
        // cb4addac): /stop / stuck-loop escalation must always produce a
        // clean slate even when the drain-timeout path had flagged the
        // entry as resumable.
        bool suspended = j.value("suspended", false);
        if (suspended) {
            auto transcript = session_dir / "transcript.jsonl";
            if (std::filesystem::exists(transcript)) {
                std::filesystem::remove(transcript);
            }
            j["suspended"] = false;
            j["resume_pending"] = false;
            j["resume_reason"] = nlohmann::json();
            j["session_id"] = key + ":" +
                              std::to_string(std::chrono::system_clock::now()
                                                 .time_since_epoch()
                                                 .count());
        }
        // resume_pending is intentionally NOT cleared here — the runner
        // calls ``consume_resume_pending`` once it has inspected the flag
        // and decided whether to inject the restart-resume system note.

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

// --- Resume-pending helpers (upstream cb4addac / c49a58a6) --------------

namespace {

// Load / save session.json under the caller-held lock.  Returns nullopt
// when the file doesn't exist or fails to parse.
std::optional<nlohmann::json> load_session_json(
    const std::filesystem::path& session_file) {
    if (!std::filesystem::exists(session_file)) return std::nullopt;
    std::ifstream in(session_file);
    if (!in) return std::nullopt;
    try {
        nlohmann::json j;
        in >> j;
        return j;
    } catch (...) {
        return std::nullopt;
    }
}

bool save_session_json(const std::filesystem::path& session_file,
                        const nlohmann::json& j) {
    std::ofstream out(session_file);
    if (!out) return false;
    out << j.dump(2);
    return true;
}

}  // namespace

std::filesystem::path SessionStore::counters_path_() const {
    return dir_ / ".restart_failure_counts.json";
}

std::unordered_map<std::string, int>
SessionStore::load_counters_locked_() const {
    std::unordered_map<std::string, int> out;
    auto p = counters_path_();
    if (!std::filesystem::exists(p)) return out;
    std::ifstream in(p);
    if (!in) return out;
    try {
        nlohmann::json j;
        in >> j;
        if (j.is_object()) {
            for (auto it = j.begin(); it != j.end(); ++it) {
                if (it.value().is_number_integer()) {
                    out[it.key()] = it.value().get<int>();
                }
            }
        }
    } catch (...) {
        // Corrupt file -> treat as empty.
    }
    return out;
}

void SessionStore::save_counters_locked_(
    const std::unordered_map<std::string, int>& counters) const {
    nlohmann::json j = nlohmann::json::object();
    for (auto& [k, v] : counters) j[k] = v;
    std::ofstream out(counters_path_());
    if (out) out << j.dump(2);
}

bool SessionStore::mark_resume_pending(const std::string& session_key,
                                         const std::string& reason) {
    std::lock_guard<std::recursive_mutex> lock(mu_);
    auto session_file = dir_ / session_key / "session.json";
    auto maybe_j = load_session_json(session_file);
    if (!maybe_j) return false;
    auto& j = *maybe_j;

    // Never override ``suspended`` — that's a hard forced-wipe signal
    // from /stop or stuck-loop escalation.
    if (j.value("suspended", false)) return false;

    j["resume_pending"] = true;
    j["resume_reason"] = reason;
    j["last_resume_marked_at"] =
        time_to_iso(std::chrono::system_clock::now());
    return save_session_json(session_file, j);
}

bool SessionStore::clear_resume_pending(const std::string& session_key) {
    std::lock_guard<std::recursive_mutex> lock(mu_);
    auto session_file = dir_ / session_key / "session.json";
    auto maybe_j = load_session_json(session_file);
    if (!maybe_j) return false;
    auto& j = *maybe_j;
    if (!j.value("resume_pending", false)) return false;
    j["resume_pending"] = false;
    j["resume_reason"] = nlohmann::json();
    j["last_resume_marked_at"] = nlohmann::json();
    save_session_json(session_file, j);
    // A successful resumed turn also resets the stuck-loop counter.
    auto counters = load_counters_locked_();
    if (counters.erase(session_key)) save_counters_locked_(counters);
    return true;
}

bool SessionStore::is_resume_pending(const std::string& session_key) const {
    std::lock_guard<std::recursive_mutex> lock(mu_);
    auto session_file = dir_ / session_key / "session.json";
    auto maybe_j = load_session_json(session_file);
    if (!maybe_j) return false;
    return maybe_j->value("resume_pending", false);
}

std::string SessionStore::resume_reason(
    const std::string& session_key) const {
    std::lock_guard<std::recursive_mutex> lock(mu_);
    auto session_file = dir_ / session_key / "session.json";
    auto maybe_j = load_session_json(session_file);
    if (!maybe_j) return {};
    return maybe_j->value("resume_reason", std::string{});
}

bool SessionStore::is_suspended(const std::string& session_key) const {
    std::lock_guard<std::recursive_mutex> lock(mu_);
    auto session_file = dir_ / session_key / "session.json";
    auto maybe_j = load_session_json(session_file);
    if (!maybe_j) return false;
    return maybe_j->value("suspended", false);
}

int SessionStore::restart_failure_count(
    const std::string& session_key) const {
    std::lock_guard<std::recursive_mutex> lock(mu_);
    auto counters = load_counters_locked_();
    auto it = counters.find(session_key);
    return it == counters.end() ? 0 : it->second;
}

int SessionStore::bump_restart_failure_count(
    const std::string& session_key) {
    std::lock_guard<std::recursive_mutex> lock(mu_);
    auto counters = load_counters_locked_();
    int next = ++counters[session_key];
    save_counters_locked_(counters);
    return next;
}

void SessionStore::clear_restart_failure_count(
    const std::string& session_key) {
    std::lock_guard<std::recursive_mutex> lock(mu_);
    auto counters = load_counters_locked_();
    if (counters.erase(session_key)) save_counters_locked_(counters);
}

SessionStore::ResumeDecision SessionStore::consume_resume_pending(
    const std::string& session_key) {
    std::lock_guard<std::recursive_mutex> lock(mu_);
    ResumeDecision d;
    auto session_file = dir_ / session_key / "session.json";
    auto maybe_j = load_session_json(session_file);
    if (!maybe_j) return d;
    auto& j = *maybe_j;
    if (!j.value("resume_pending", false)) return d;

    d.reason = j.value("resume_reason", std::string{});

    // Check stuck-loop threshold (PR #7536, mirror cb4addac escalation).
    auto counters = load_counters_locked_();
    int count = counters.count(session_key) ? counters.at(session_key) : 0;
    d.failure_count = count;

    if (count >= kResumeFailureThreshold) {
        // Escalate to suspended: wipe the transcript and clear the flag
        // so the next inbound message gets a fresh session_id.
        auto transcript = dir_ / session_key / "transcript.jsonl";
        if (std::filesystem::exists(transcript)) {
            std::filesystem::remove(transcript);
        }
        j["suspended"] = true;
        j["resume_pending"] = false;
        j["resume_reason"] = nlohmann::json();
        save_session_json(session_file, j);
        counters.erase(session_key);
        save_counters_locked_(counters);
        d.escalated_to_suspended = true;
        d.should_resume = false;
        return d;
    }

    d.should_resume = true;
    // Bump the counter optimistically — the runner calls
    // ``clear_resume_pending`` only on a successful turn, which also
    // resets the counter.  If the resumed turn crashes / drain-timeouts
    // again, the counter persists and escalation kicks in on the
    // (kResumeFailureThreshold+1)th attempt.
    counters[session_key] = count + 1;
    save_counters_locked_(counters);
    return d;
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
