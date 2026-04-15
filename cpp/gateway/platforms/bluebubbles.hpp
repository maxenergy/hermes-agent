// Phase 12 — BlueBubbles (iMessage bridge) platform adapter.
//
// Depth port of gateway/platforms/bluebubbles.py (936 LoC).
// Covers:
//   * Server URL normalization + password query-parameter embedding.
//   * Webhook registration + deregistration (idempotent).
//   * Chat GUID resolution (handle -> GUID cache).
//   * Private-API feature detection.
//   * Outbound text/attachment (image/video/voice/file) message builders.
//   * Tapback reaction + typing-indicator + read-receipt endpoints.
//   * Inbound webhook parser (new-message/updated-message/typing/read).
//   * Markdown stripping for iMessage plain-text delivery.
//   * SMS-bridge routing + group chat participants.
#pragma once

#include <chrono>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <nlohmann/json.hpp>

#include <hermes/gateway/gateway_runner.hpp>
#include <hermes/llm/llm_client.hpp>

namespace hermes::gateway::platforms {

inline constexpr const char* kBbDefaultWebhookHost = "127.0.0.1";
inline constexpr int kBbDefaultWebhookPort = 8765;
inline constexpr const char* kBbDefaultWebhookPath = "/bluebubbles/webhook";
inline constexpr std::size_t kBbMaxTextLength = 10000;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

// Redact phone numbers and email addresses from a string for logging.
std::string bb_redact(const std::string& text);

// Ensure URL has scheme + no trailing slash.
std::string bb_normalize_server_url(const std::string& raw);

// Remove common markdown formatting for plain-text iMessage delivery.
std::string bb_strip_markdown(const std::string& text);

// URL-encode a string for path/query placement (safe="").
std::string bb_url_quote(const std::string& s);

// Detect whether a handle looks like an email or phone number (used to
// trigger chat-creation when the resolved GUID is missing).
bool bb_looks_like_handle(const std::string& s);

// Detect whether a chat_id is a group chat (contains ";+;" marker).
bool bb_is_group_chat(const std::string& chat_id);

// Classify an inbound webhook type (message / updated / read / typing /
// reaction / unknown).
std::string bb_classify_event_type(const nlohmann::json& payload);

// Extract the attachment list from a new-message webhook (returns vector
// of {guid, mime_type, transfer_name} tuples encoded as JSON).
nlohmann::json bb_extract_attachments(const nlohmann::json& message);

// Normalize a reaction token ("love", "laugh", "emphasize", "like",
// "dislike", "question", empty for unknown).
std::string bb_normalize_reaction(const std::string& token);

// ---------------------------------------------------------------------------
// Adapter
// ---------------------------------------------------------------------------

class BlueBubblesAdapter : public BasePlatformAdapter {
public:
    struct Config {
        std::string server_url;
        std::string password;
        std::string webhook_host = kBbDefaultWebhookHost;
        int webhook_port = kBbDefaultWebhookPort;
        std::string webhook_path = kBbDefaultWebhookPath;
        bool send_read_receipts = true;

        // DM / group policy — same shape as other platforms.
        std::vector<std::string> allow_from;
        std::vector<std::string> blocked_handles;
        bool default_allow_dm = true;
    };

    struct SendResult {
        bool success = false;
        std::string message_id;
        std::string error;
        nlohmann::json raw;
    };

    explicit BlueBubblesAdapter(Config cfg);
    BlueBubblesAdapter(Config cfg, hermes::llm::HttpTransport* transport);
    ~BlueBubblesAdapter() override = default;

    // ----- BasePlatformAdapter ---------------------------------------------
    Platform platform() const override { return Platform::BlueBubbles; }
    bool connect() override;
    void disconnect() override;
    bool send(const std::string& chat_id, const std::string& content) override;
    void send_typing(const std::string& chat_id) override;
    AdapterErrorKind last_error_kind() const override { return last_error_kind_; }

    // ----- URL / credential plumbing ---------------------------------------
    std::string api_url(const std::string& path) const;
    std::string webhook_external_url() const;

    // ----- Core API --------------------------------------------------------
    // Ping + /server/info probe used during connect().
    bool probe_server();
    bool private_api_enabled() const { return private_api_enabled_; }
    bool helper_connected() const { return helper_connected_; }

    // Webhook registration (idempotent — reuses existing entry when found).
    bool register_webhook();
    bool unregister_webhook();
    std::vector<nlohmann::json> find_registered_webhooks(const std::string& url);

    // Resolve a chat target (chat_id or handle) to a BlueBubbles chatGuid.
    // Caches the mapping in guid_cache_.
    std::optional<std::string> resolve_chat_guid(const std::string& target);

    // Create a new chat when the target is an email/phone handle and the
    // Private API is available.  Returns the new chat GUID.
    std::optional<std::string> create_chat_for_handle(const std::string& handle,
                                                      const std::string& text);

    // ----- Outbound --------------------------------------------------------
    SendResult send_text(const std::string& chat_id, const std::string& text,
                         const std::optional<std::string>& reply_to = std::nullopt);
    SendResult send_attachment(const std::string& chat_id,
                               const std::string& file_path,
                               const std::string& filename = {},
                               const std::string& caption = {},
                               bool is_audio_message = false);
    SendResult send_reaction(const std::string& chat_id,
                             const std::string& message_guid,
                             const std::string& reaction,
                             int part_index = 0);
    bool mark_read(const std::string& chat_id);
    bool stop_typing(const std::string& chat_id);

    // ----- Chat info -------------------------------------------------------
    nlohmann::json get_chat_info(const std::string& chat_id);

    // ----- Inbound --------------------------------------------------------
    // Parse a webhook POST body into a MessageEvent.  Returns std::nullopt
    // for events we don't propagate (typing, read receipts, or filtered).
    std::optional<MessageEvent> parse_webhook_body(const nlohmann::json& payload);

    // Download an attachment by GUID; returns the raw bytes.
    std::string download_attachment(const std::string& att_guid);

    // ----- Helpers for unit tests ------------------------------------------
    static std::string temp_guid_for(std::chrono::system_clock::time_point now);
    std::size_t guid_cache_size() const;
    void clear_guid_cache();
    nlohmann::json build_text_payload(const std::string& chat_guid,
                                      const std::string& text,
                                      const std::optional<std::string>& reply_to = std::nullopt) const;

    // ----- Accessors -------------------------------------------------------
    const Config& config() const { return cfg_; }
    bool connected() const { return connected_; }

private:
    hermes::llm::HttpTransport* get_transport();
    nlohmann::json api_get(const std::string& path);
    nlohmann::json api_post(const std::string& path,
                            const nlohmann::json& payload);
    nlohmann::json api_delete(const std::string& path);

    Config cfg_;
    hermes::llm::HttpTransport* transport_ = nullptr;
    bool connected_ = false;
    bool private_api_enabled_ = false;
    bool helper_connected_ = false;
    AdapterErrorKind last_error_kind_ = AdapterErrorKind::None;
    std::string last_error_;

    mutable std::mutex guid_mu_;
    std::unordered_map<std::string, std::string> guid_cache_;
};

}  // namespace hermes::gateway::platforms
