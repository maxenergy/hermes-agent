// Phase 12 — Feishu (Lark) platform adapter (full-depth port of
// gateway/platforms/feishu.py).
//
// Ports:
//   - OAuth tenant_access_token acquisition + rotation
//   - Event subscription verification (challenge) + encrypt handshake
//   - AES-256-CBC decrypt for encrypted events (SHA256(app_secret) key;
//     IV prepended to the ciphertext, PKCS#7 padding)
//   - Event schema routing: message send/recall/reaction/menu/card/chat
//   - Interactive cards (builder + approval flow)
//   - File/image upload/download helpers
//   - Group vs. single-chat policy resolution with @mention gating
//   - Message segmentation (Feishu ~4000 char practical limit)
//   - Rate-limit handling (sliding-window per-IP)
//   - Pagination for group membership queries
//   - User/bot identifier mapping
//   - Permission scopes
//
// The public API (BasePlatformAdapter::connect / disconnect / send /
// send_typing, plus build_card_message and the Config struct) is kept
// stable — everything else is additive.
#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include <hermes/gateway/gateway_runner.hpp>
#include <hermes/llm/llm_client.hpp>

namespace hermes::gateway::platforms {

// ---------------------------------------------------------------------------
// Utility enums & constants
// ---------------------------------------------------------------------------

enum class FeishuConnectionMode {
    WebSocket,
    Webhook,
    Unknown,
};

enum class FeishuGroupPolicy {
    Open,           // any member may trigger the bot
    Allowlist,      // only listed users
    Blacklist,      // everyone except listed users
    AdminOnly,      // bot-level admins only
    Disabled,       // do not respond
    Unknown,
};

enum class FeishuErrorKind {
    None,
    Transient,      // 5xx / network / 99991400 rate-limited
    RateLimited,    // 99991400 series
    Unauthorized,   // 99991663 (invalid token) / 99991664
    ReplyMissing,   // 230011 / 231003 — fallback to fresh create
    BadRequest,
    Fatal,
};

struct FeishuError {
    FeishuErrorKind kind = FeishuErrorKind::None;
    int http_status = 0;
    int feishu_code = 0;  // top-level "code" field
    std::string message;
    double retry_after_seconds = 0.0;
};

enum class FeishuMessageType {
    Text,
    Post,
    Image,
    File,
    Audio,
    Media,
    Sticker,
    Interactive,
    ShareChat,
    MergeForward,
    System,
    Unknown,
};

FeishuConnectionMode parse_connection_mode(std::string_view s);
std::string to_string(FeishuConnectionMode m);
FeishuGroupPolicy parse_group_policy(std::string_view s);
std::string to_string(FeishuGroupPolicy p);
FeishuMessageType parse_message_type(std::string_view s);
std::string to_string(FeishuMessageType t);

// ---------------------------------------------------------------------------
// Data classes mirrored from the Python dataclasses
// ---------------------------------------------------------------------------

struct FeishuPostMediaRef {
    std::string file_key;
    std::string file_name;
    std::string resource_type = "file";  // "file" | "audio" | "video"
};

struct FeishuPostParseResult {
    std::string text_content;
    std::vector<std::string> image_keys;
    std::vector<FeishuPostMediaRef> media_refs;
    std::vector<std::string> mentioned_ids;
};

struct FeishuNormalizedMessage {
    std::string raw_type;
    std::string text_content;
    std::string preferred_message_type = "text";
    std::vector<std::string> image_keys;
    std::vector<FeishuPostMediaRef> media_refs;
    std::vector<std::string> mentioned_ids;
    std::string relation_kind = "plain";
    nlohmann::json metadata = nlohmann::json::object();
};

struct FeishuGroupRule {
    FeishuGroupPolicy policy = FeishuGroupPolicy::Open;
    std::unordered_set<std::string> allowlist;
    std::unordered_set<std::string> blacklist;
};

// ---------------------------------------------------------------------------
// Free-function helpers (pure text utilities — direct ports of Python
// module-level helpers). These are declared public so tests can cover them.
// ---------------------------------------------------------------------------

std::string escape_markdown_text(const std::string& text);
std::string wrap_inline_code(const std::string& text);
std::string sanitize_fence_language(const std::string& language);
std::string normalize_feishu_text(const std::string& text);
std::string strip_markdown_to_plain_text(const std::string& text);
std::vector<std::string> unique_lines(const std::vector<std::string>& lines);

// Optional<int> helpers with defaulting/minimum constraints.
std::optional<long long> coerce_int(const nlohmann::json& value,
                                    long long min_value = 0);
long long coerce_required_int(const nlohmann::json& value, long long def,
                              long long min_value = 0);

// Post payload builders / parsers.
std::string build_markdown_post_payload(const std::string& content);
FeishuPostParseResult parse_feishu_post_content(const std::string& raw);
FeishuPostParseResult parse_feishu_post_payload(const nlohmann::json& payload);

// Normalize inbound messages by type.
FeishuNormalizedMessage normalize_feishu_message(const std::string& message_type,
                                                 const std::string& raw_content);

// Chunk a rendered message for Feishu's ~8000 char ceiling while avoiding
// the ~4096 client-side split threshold.
std::vector<std::string> split_message_for_feishu(const std::string& content,
                                                  std::size_t max_len = 8000);

// Classify an error given an HTTP status + parsed response body JSON.
FeishuError classify_feishu_error(int http_status,
                                  const nlohmann::json& body);

// Normalize various representations into the canonical chat type strings:
// "private" | "group" | "channel" | "unknown".
std::string normalize_chat_type(const std::string& raw);

// Low-level primitives exposed for testing.
std::string feishu_base64_decode(std::string_view input);
std::string feishu_base64_encode(std::string_view bytes);
std::string sha256_bytes(std::string_view bytes);

// AES-256-CBC decryption using the Feishu encrypt_key scheme: the key is
// SHA256(encrypt_key); the first 16 bytes of the ciphertext are the IV;
// PKCS#7 padding; returns the cleartext or an empty optional on failure.
std::optional<std::string> feishu_aes_decrypt(const std::string& encrypt_key,
                                              const std::string& base64_ct);

// Rate-limit helpers (sliding window).
class FeishuRateLimiter {
public:
    explicit FeishuRateLimiter(std::size_t max_keys = 4096)
        : max_keys_(max_keys) {}

    // Returns true when `key` is allowed; false if it has exceeded `max_per_window`
    // in the rolling `window_seconds`.
    bool allow(const std::string& key,
               std::size_t max_per_window = 120,
               double window_seconds = 60.0,
               std::chrono::steady_clock::time_point now =
                   std::chrono::steady_clock::now());

    std::size_t tracked_keys() const;
    void clear();

private:
    struct Entry {
        std::size_t count = 0;
        std::chrono::steady_clock::time_point window_start;
    };
    mutable std::mutex mu_;
    std::unordered_map<std::string, Entry> counts_;
    std::size_t max_keys_;
};

// Webhook anomaly tracker — records consecutive error responses per
// remote IP and reports whether the threshold has been crossed.
class FeishuAnomalyTracker {
public:
    explicit FeishuAnomalyTracker(std::size_t threshold = 25,
                                  double ttl_seconds = 6 * 60 * 60.0)
        : threshold_(threshold), ttl_seconds_(ttl_seconds) {}

    // Returns true the FIRST time the threshold is crossed for `ip`.
    bool record(const std::string& ip,
                const std::string& status,
                std::chrono::steady_clock::time_point now =
                    std::chrono::steady_clock::now());
    void clear(const std::string& ip);
    std::size_t tracked_ips() const;

private:
    struct Entry {
        std::size_t count = 0;
        std::string last_status;
        std::chrono::steady_clock::time_point first_seen;
        bool alerted = false;
    };
    mutable std::mutex mu_;
    std::unordered_map<std::string, Entry> entries_;
    std::size_t threshold_;
    double ttl_seconds_;
};

// Simple LRU-ish dedup cache (matches Python _seen_message_ids behaviour).
class FeishuDedupCache {
public:
    explicit FeishuDedupCache(std::size_t cap = 2048,
                              double ttl_seconds = 24 * 60 * 60.0)
        : cap_(cap), ttl_(ttl_seconds) {}

    // Returns true if this message_id has not been seen before (and records it);
    // false if it is a duplicate within the TTL window.
    bool check_and_add(const std::string& message_id,
                       std::chrono::steady_clock::time_point now =
                           std::chrono::steady_clock::now());
    std::size_t size() const;
    void clear();

private:
    mutable std::mutex mu_;
    std::unordered_map<std::string, std::chrono::steady_clock::time_point> seen_;
    std::deque<std::string> order_;
    std::size_t cap_;
    double ttl_;
};

// ---------------------------------------------------------------------------
// FeishuAdapter — the platform adapter itself.
// ---------------------------------------------------------------------------

class FeishuAdapter : public BasePlatformAdapter {
public:
    struct Config {
        // Required credentials.
        std::string app_id;
        std::string app_secret;

        // Endpoint domain ("feishu" or "lark") — selects base URL.
        std::string domain = "feishu";

        // Connection mode — "websocket" (default) or "webhook".
        std::string connection_mode = "websocket";

        // Webhook verification.
        std::string encrypt_key;
        std::string verification_token;
        std::string webhook_host = "127.0.0.1";
        int webhook_port = 8765;
        std::string webhook_path = "/feishu/webhook";

        // Group/permission policy.
        std::string group_policy = "allowlist";
        std::string default_group_policy;
        std::unordered_set<std::string> allowed_group_users;
        std::unordered_set<std::string> admins;
        std::unordered_map<std::string, FeishuGroupRule> group_rules;

        // Bot identity (hydrated from API during connect()).
        std::string bot_open_id;
        std::string bot_user_id;
        std::string bot_name;

        // Rate-limit / batching tuning.
        std::size_t dedup_cache_size = 2048;
        double text_batch_delay_seconds = 0.6;
        double text_batch_split_delay_seconds = 2.0;
        std::size_t text_batch_max_messages = 8;
        std::size_t text_batch_max_chars = 4000;
        double media_batch_delay_seconds = 0.8;
        int max_send_retries = 3;

        // Reaction emoji (persistent ACK).
        std::string ack_emoji = "OK";
    };

    static constexpr std::size_t kMaxMessageLength = 8000;
    static constexpr std::size_t kSplitThreshold = 4000;
    static constexpr int kBotMsgTrackSize = 512;

    explicit FeishuAdapter(Config cfg);
    FeishuAdapter(Config cfg, hermes::llm::HttpTransport* transport);
    ~FeishuAdapter() override = default;

    // ----- BasePlatformAdapter --------------------------------------------
    Platform platform() const override { return Platform::Feishu; }
    bool connect() override;
    void disconnect() override;
    bool send(const std::string& chat_id, const std::string& content) override;
    void send_typing(const std::string& chat_id) override;
    AdapterErrorKind last_error_kind() const override;

    // ----- URL composition ------------------------------------------------
    std::string base_url() const;
    std::string auth_url() const;
    std::string messages_url(const std::string& receive_id_type) const;
    std::string message_url(const std::string& message_id) const;
    std::string reply_url(const std::string& message_id) const;
    std::string update_url(const std::string& message_id) const;
    std::string reaction_url(const std::string& message_id) const;
    std::string chat_info_url(const std::string& chat_id) const;
    std::string chat_members_url(const std::string& chat_id,
                                 const std::string& page_token = "") const;
    std::string image_upload_url() const;
    std::string file_upload_url() const;
    std::string message_resource_url(const std::string& message_id,
                                     const std::string& file_key,
                                     const std::string& type) const;

    // ----- Auth / token management ----------------------------------------
    bool refresh_tenant_access_token();
    std::string tenant_access_token() const;
    void set_tenant_access_token(std::string tok,
                                 std::chrono::seconds ttl = std::chrono::hours(2));
    bool access_token_expired(
        std::chrono::steady_clock::time_point now =
            std::chrono::steady_clock::now()) const;
    std::unordered_map<std::string, std::string> auth_headers() const;

    // ----- Event-subscription verification + decrypt ----------------------
    // Handles both the URL-verification challenge and the "encrypt"
    // handshake in a single call.  On success writes the plain-text event
    // payload to `out` and returns the appropriate HTTP response body.
    // Returns the JSON body the webhook handler should send back to Feishu.
    nlohmann::json handle_webhook_body(const nlohmann::json& body,
                                       nlohmann::json* decrypted_event) const;

    // Verify the `token` field against the configured verification_token.
    bool verify_token(const std::string& token) const;

    // ----- Inbound event dispatch (synthetic conversion) ------------------
    // Converts a decoded event payload into a MessageEvent suitable for
    // feeding into GatewayRunner::handle_message.
    std::optional<MessageEvent> event_to_message(
        const nlohmann::json& event) const;

    // Classify the event schema name.  Returns one of: "message", "reaction",
    // "card_action", "recall", "read", "menu_click", "bot_added",
    // "bot_removed", "chat_disbanded", "unknown".
    static std::string classify_event(const nlohmann::json& event);

    // ----- Outbound helpers -----------------------------------------------
    // Build a raw Feishu "content" JSON string for the given kind.
    static std::string build_text_content(const std::string& text);
    static std::string build_post_content(const std::string& markdown);
    static std::string build_image_content(const std::string& image_key);
    static std::string build_file_content(const std::string& file_key,
                                          const std::string& file_name = "");
    static std::string build_audio_content(const std::string& file_key);

    // Build a Feishu interactive card message JSON.
    static std::string build_card_message(const std::string& title,
                                          const std::string& content);
    static nlohmann::json build_approval_card(const std::string& command,
                                              const std::string& description,
                                              long long approval_id,
                                              const std::string& header_title =
                                                  "Command Approval Required");
    static nlohmann::json build_menu_card(
        const std::string& title,
        const std::vector<std::pair<std::string, std::string>>& items);

    // Classify the outbound message type to dispatch (text / post / interactive).
    std::pair<std::string, std::string> build_outbound_payload(
        const std::string& content) const;

    // Send a text message.  Returns the message_id on success.
    struct SendResult {
        bool ok = false;
        std::string message_id;
        FeishuError error;
    };
    SendResult send_message(const std::string& chat_id,
                            const std::string& content,
                            const std::string& receive_id_type = "chat_id",
                            std::optional<std::string> reply_to = std::nullopt);

    SendResult send_post(const std::string& chat_id,
                         const std::string& content,
                         const std::string& receive_id_type = "chat_id");

    SendResult send_card(const std::string& chat_id,
                         const nlohmann::json& card,
                         const std::string& receive_id_type = "chat_id");

    SendResult edit_message(const std::string& message_id,
                            const std::string& content);

    bool recall_message(const std::string& message_id);

    SendResult reply_to_message(const std::string& message_id,
                                const std::string& content,
                                bool reply_in_thread = false);

    bool add_reaction(const std::string& message_id,
                      const std::string& emoji = "OK");
    bool remove_reaction(const std::string& message_id,
                         const std::string& reaction_id);

    // File/image upload — returns the new image_key/file_key on success.
    std::optional<std::string> upload_image(const std::string& image_type,
                                            const std::string& bytes);
    std::optional<std::string> upload_file(const std::string& file_type,
                                           const std::string& file_name,
                                           const std::string& bytes);

    // Download a resource attached to an inbound message.  Returns the raw
    // bytes (empty on failure) and writes the suggested filename to
    // *out_filename when non-null.
    std::string download_message_resource(const std::string& message_id,
                                          const std::string& file_key,
                                          const std::string& type,
                                          std::string* out_filename = nullptr);

    // ----- Chat lookup / membership ---------------------------------------
    nlohmann::json get_chat_info(const std::string& chat_id);

    // Enumerate all chat members via cursor pagination.
    std::vector<nlohmann::json> list_chat_members(const std::string& chat_id);

    // ----- Policy gating --------------------------------------------------
    bool allow_group_message(const std::string& sender_id,
                             const std::string& chat_id) const;
    bool should_accept_group_message(const nlohmann::json& message,
                                     const std::string& sender_id,
                                     const std::string& chat_id) const;
    bool message_mentions_bot(const nlohmann::json& mentions) const;

    // ----- Approval flow (card-button → agent thread bridge) --------------
    long long register_approval(std::string session_key, std::string chat_id,
                                std::string message_id = "");
    struct ApprovalSlot {
        std::string session_key;
        std::string chat_id;
        std::string message_id;
    };
    std::optional<ApprovalSlot> take_approval(long long approval_id);

    // ----- Dedup & rate limiting ------------------------------------------
    FeishuDedupCache& dedup_cache() { return dedup_cache_; }
    FeishuRateLimiter& rate_limiter() { return rate_limiter_; }
    FeishuAnomalyTracker& anomaly_tracker() { return anomaly_tracker_; }

    // ----- Accessors ------------------------------------------------------
    const Config& config() const { return cfg_; }
    bool connected() const { return connected_; }
    hermes::llm::HttpTransport* transport() { return get_transport(); }

private:
    hermes::llm::HttpTransport* get_transport();

    // Perform a POST with the tenant_access_token attached, refreshing on
    // 401-style failures.  Parses response and classifies errors.
    nlohmann::json call_api(const std::string& method_name,
                            const std::string& url,
                            const nlohmann::json& payload,
                            FeishuError* out_error = nullptr,
                            bool is_get = false);

    std::string api_domain() const;

    Config cfg_;
    hermes::llm::HttpTransport* transport_ = nullptr;

    mutable std::mutex token_mu_;
    std::string tenant_access_token_;
    std::chrono::steady_clock::time_point token_expires_at_{};

    std::atomic<bool> connected_{false};
    mutable std::mutex err_mu_;
    FeishuError last_error_;

    FeishuDedupCache dedup_cache_;
    FeishuRateLimiter rate_limiter_;
    FeishuAnomalyTracker anomaly_tracker_;

    mutable std::mutex approval_mu_;
    long long approval_counter_ = 0;
    std::unordered_map<long long, ApprovalSlot> approval_state_;

    // LRU of message_ids we've sent — used to route inbound reactions back
    // to the originating chat (per the Python _sent_message_ids_to_chat).
    mutable std::mutex sent_mu_;
    std::unordered_map<std::string, std::string> sent_message_chats_;
    std::deque<std::string> sent_order_;

    // Card-action dedup tokens (LRU w/ TTL).
    FeishuDedupCache card_action_dedup_{1024, 15 * 60.0};
};

}  // namespace hermes::gateway::platforms
