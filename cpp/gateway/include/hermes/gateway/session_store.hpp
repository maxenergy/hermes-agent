// File-based session persistence for gateway.
#pragma once

#include <chrono>
#include <filesystem>
#include <map>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include <hermes/gateway/gateway_config.hpp>

namespace hermes::gateway {

struct SessionSource {
    Platform platform;
    std::string chat_id, chat_name, chat_type;  // dm|group|channel|thread
    std::string user_id, user_name;
    std::string thread_id, chat_topic;
    std::string user_id_alt, chat_id_alt;  // Signal UUID etc.

    std::string description() const;
    nlohmann::json to_json() const;
    static SessionSource from_json(const nlohmann::json& j);
};

struct SessionContext {
    SessionSource source;
    std::vector<Platform> connected_platforms;
    std::map<Platform, HomeChannel> home_channels;
    std::string session_key, session_id;
    std::chrono::system_clock::time_point created_at, updated_at;

    nlohmann::json to_json() const;
};

class SessionStore {
public:
    explicit SessionStore(std::filesystem::path sessions_dir);

    std::string get_or_create_session(const SessionSource& source);
    void reset_session(const std::string& session_key);
    void append_message(const std::string& session_key,
                        const nlohmann::json& message);
    std::vector<nlohmann::json> load_transcript(
        const std::string& session_key);

    bool should_reset(const std::string& session_key,
                      const SessionResetPolicy& policy);

    // PII redaction: hash IDs for Telegram/Signal/WhatsApp/BlueBubbles
    // Discord excluded (needs real IDs for <@user_id> mentions)
    static std::string hash_id(std::string_view id);  // SHA256 12-char hex
    SessionSource redact_pii(const SessionSource& source) const;

private:
    std::filesystem::path dir_;
    mutable std::recursive_mutex mu_;
    // Each session: {dir}/{session_key}/session.json + transcript.jsonl
};

}  // namespace hermes::gateway
