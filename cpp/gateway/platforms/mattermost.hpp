// Phase 12 — Mattermost platform adapter (depth port).
//
// Mirrors gateway/platforms/mattermost.py.  Covers credential plumbing,
// REST helpers (GET/POST/PUT), channel-type mapping, mention gating,
// dedup cache, posting (with thread support), edit, file upload payload
// shape, typing, ws URL derivation, and event parsing.
#pragma once

#include <chrono>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <nlohmann/json.hpp>

#include <hermes/gateway/gateway_runner.hpp>
#include <hermes/llm/llm_client.hpp>

namespace hermes::gateway::platforms {

inline constexpr std::size_t kMmMaxPostLength = 4000;
inline constexpr std::size_t kMmDedupMax = 2000;
inline constexpr std::chrono::seconds kMmDedupTtl{300};
inline constexpr double kMmReconnectBaseDelay = 2.0;
inline constexpr double kMmReconnectMaxDelay = 60.0;
inline constexpr double kMmReconnectJitter = 0.2;

// Map a Mattermost channel-type code to a SessionSource chat_type.
std::string mm_channel_type_to_chat_type(const std::string& code);

// Convert a base URL (http/https) to its WebSocket variant + websocket path.
std::string mm_websocket_url(const std::string& base_url);

// Strip image-markdown: ![alt](url) → url (Mattermost previews bare URLs).
std::string mm_strip_image_markdown(const std::string& content);

// Build the authentication_challenge envelope for ws handshake.
nlohmann::json mm_build_auth_challenge(const std::string& token, int seq = 1);

// Build the JSON payload for posting a message (with optional thread root).
nlohmann::json mm_build_post_payload(const std::string& channel_id,
                                     const std::string& message,
                                     const std::string& root_id = "");

nlohmann::json mm_build_post_with_files_payload(
    const std::string& channel_id, const std::string& caption,
    const std::vector<std::string>& file_ids,
    const std::string& root_id = "");

// Decide whether an incoming non-DM channel post mentions the bot.
bool mm_message_mentions_bot(const std::string& message,
                             const std::string& bot_user_id,
                             const std::string& bot_username);

// Strip @mentions from a non-DM message.
std::string mm_strip_mentions(const std::string& message,
                              const std::string& bot_user_id,
                              const std::string& bot_username);

// Parse a raw "posted" WS event payload into a structured form.  The "post"
// field is JSON-encoded as a string in the WS protocol — this helper
// performs that nested parse.
struct MmPostedEvent {
    std::string event_type;
    std::string channel_type_raw;
    std::string sender_name;
    nlohmann::json post;  // parsed inner post object
    bool valid = false;
};
MmPostedEvent mm_parse_posted_event(const nlohmann::json& event);

// Determine the message type from file mime types + text body.
std::string mm_classify_message_type(const std::string& text,
                                     const std::vector<std::string>& mime_types);

class MattermostAdapter : public BasePlatformAdapter {
public:
    struct Config {
        std::string token;
        std::string url;
        std::string reply_mode = "off";  // "thread" or "off"
        bool require_mention = true;
        std::unordered_set<std::string> free_response_channels;
    };

    explicit MattermostAdapter(Config cfg);
    MattermostAdapter(Config cfg, hermes::llm::HttpTransport* transport);

    // ----- BasePlatformAdapter ---------------------------------------------
    Platform platform() const override { return Platform::Mattermost; }
    bool connect() override;
    void disconnect() override;
    bool send(const std::string& chat_id, const std::string& content) override;
    void send_typing(const std::string& chat_id) override;
    AdapterErrorKind last_error_kind() const override { return last_error_kind_; }

    // ----- REST helpers ---------------------------------------------------
    nlohmann::json api_get(const std::string& path);
    nlohmann::json api_post(const std::string& path, const nlohmann::json& payload);
    nlohmann::json api_put(const std::string& path, const nlohmann::json& payload);

    // ----- Posts ----------------------------------------------------------
    std::string create_post(const std::string& channel_id, const std::string& message,
                            const std::string& reply_to = "");
    bool edit_post(const std::string& post_id, const std::string& message);
    nlohmann::json get_chat_info(const std::string& channel_id);

    // ----- Dedup ---------------------------------------------------------
    bool seen_post(const std::string& post_id);
    void mark_seen(const std::string& post_id);
    void prune_seen();
    std::size_t seen_size() const;

    // ----- Identity ------------------------------------------------------
    void set_bot_identity(std::string user_id, std::string username);
    const std::string& bot_user_id() const { return bot_user_id_; }
    const std::string& bot_username() const { return bot_username_; }

    // ----- Accessors -----------------------------------------------------
    const Config& config() const { return cfg_; }
    bool connected() const { return connected_; }

private:
    hermes::llm::HttpTransport* get_transport();
    std::unordered_map<std::string, std::string> auth_headers() const;

    Config cfg_;
    hermes::llm::HttpTransport* transport_ = nullptr;
    bool connected_ = false;
    AdapterErrorKind last_error_kind_ = AdapterErrorKind::None;
    std::string bot_user_id_;
    std::string bot_username_;

    mutable std::mutex seen_mu_;
    std::unordered_map<std::string, std::chrono::system_clock::time_point>
        seen_posts_;
};

}  // namespace hermes::gateway::platforms
