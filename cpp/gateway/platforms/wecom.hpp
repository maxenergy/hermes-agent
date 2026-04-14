// Phase 12 — WeCom (Enterprise WeChat) platform adapter.
//
// Depth port of gateway/platforms/wecom.py (1435 LoC).
// Covers:
//   * WebSocket subscribe/heartbeat/dispatch state machine.
//   * aibot_subscribe handshake + request-response plumbing.
//   * Inbound event parsing (text / markdown / image / file / voice /
//     video / template-card / menu-click / external-contact events).
//   * Corporate-API REST fallback (access_token rotation, group
//     webhooks, markdown / card / textcard / template_card messages,
//     approval events, department tree, menu actions).
//   * Chunked media upload state (init / chunk / finish).
//   * Allowlist policy resolution (open / allowlist / disabled / pairing).
//   * Group vs DM routing with @mention gating.
//   * Dedup cache (msg_id window).
//   * Text batching (late-merge follow-ups to keep one agent turn).
//
// The adapter exposes a synchronous-facing API; the WebSocket event
// loop is expected to be driven by the gateway runner.
#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
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
// Protocol constants.
// ---------------------------------------------------------------------------

inline constexpr const char* kWeComDefaultWs = "wss://openws.work.weixin.qq.com";

inline constexpr const char* kWeComCmdSubscribe = "aibot_subscribe";
inline constexpr const char* kWeComCmdCallback = "aibot_msg_callback";
inline constexpr const char* kWeComCmdLegacyCallback = "aibot_callback";
inline constexpr const char* kWeComCmdEventCallback = "aibot_event_callback";
inline constexpr const char* kWeComCmdSend = "aibot_send_msg";
inline constexpr const char* kWeComCmdRespond = "aibot_respond_msg";
inline constexpr const char* kWeComCmdPing = "ping";
inline constexpr const char* kWeComCmdUploadInit = "aibot_upload_media_init";
inline constexpr const char* kWeComCmdUploadChunk = "aibot_upload_media_chunk";
inline constexpr const char* kWeComCmdUploadFinish = "aibot_upload_media_finish";

inline constexpr std::size_t kWeComMaxMessageLength = 4000;
inline constexpr std::size_t kWeComSplitThreshold = 3900;

inline constexpr std::size_t kWeComImageMaxBytes = 10 * 1024 * 1024;
inline constexpr std::size_t kWeComVideoMaxBytes = 10 * 1024 * 1024;
inline constexpr std::size_t kWeComVoiceMaxBytes = 2 * 1024 * 1024;
inline constexpr std::size_t kWeComFileMaxBytes = 20 * 1024 * 1024;
inline constexpr std::size_t kWeComUploadChunkSize = 512 * 1024;
inline constexpr std::size_t kWeComMaxUploadChunks = 100;

inline constexpr double kWeComConnectTimeoutSec = 20.0;
inline constexpr double kWeComRequestTimeoutSec = 15.0;
inline constexpr double kWeComHeartbeatIntervalSec = 30.0;

inline constexpr std::size_t kWeComDedupMax = 1000;
inline constexpr double kWeComDedupWindowSec = 300.0;

// ---------------------------------------------------------------------------
// Helpers (module-level in Python).
// ---------------------------------------------------------------------------

// Coerce a CSV/list value into a trimmed string vector.
std::vector<std::string> wecom_coerce_list(const nlohmann::json& value);

// Strip `wecom:user:`/`wecom:group:` prefixes.
std::string wecom_normalize_entry(const std::string& raw);

// Case-insensitive allowlist check with `*` wildcard.
bool wecom_entry_matches(const std::vector<std::string>& entries,
                         const std::string& target);

// Detect image extension from byte magic (PNG / JPEG / GIF / WEBP).
std::string wecom_detect_image_ext(const std::string& bytes);

// Guess MIME by extension.
std::string wecom_mime_for_ext(const std::string& ext,
                               const std::string& fallback = "application/octet-stream");

// Guess extension from URL + Content-Type.
std::string wecom_guess_extension(const std::string& url,
                                  const std::string& content_type,
                                  const std::string& fallback = ".bin");

// Guess filename from URL + Content-Disposition.
std::string wecom_guess_filename(const std::string& url,
                                 const std::string& content_disposition,
                                 const std::string& content_type);

// Media-type classification (image / video / voice / file).
std::string wecom_detect_media_type(const std::string& content_type);

// Apply the per-type size limits; returns true if within bounds.  Writes an
// error string on failure.
bool wecom_apply_size_limits(std::size_t file_size,
                             const std::string& detected_type,
                             std::string* err_out = nullptr);

// Split a message body into <=max-length chunks on paragraph/line boundaries.
std::vector<std::string> wecom_split_text(const std::string& content,
                                          std::size_t max_length = kWeComMaxMessageLength);

// Derive a MessageEvent message_type string from body+media classification.
std::string wecom_derive_message_type(const nlohmann::json& body,
                                      const std::string& text,
                                      const std::vector<std::string>& media_types);

// ---------------------------------------------------------------------------
// Dedup cache (time-windowed).
// ---------------------------------------------------------------------------

class WeComDedupCache {
public:
    explicit WeComDedupCache(std::size_t cap = kWeComDedupMax,
                             double ttl_seconds = kWeComDedupWindowSec)
        : cap_(cap), ttl_(ttl_seconds) {}
    bool check_and_add(const std::string& msg_id,
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
// Policy / group rules.
// ---------------------------------------------------------------------------

enum class WeComDmPolicy {
    Open,
    Allowlist,
    Disabled,
    Pairing,
};

enum class WeComGroupPolicy {
    Open,
    Allowlist,
    Disabled,
};

WeComDmPolicy parse_wecom_dm_policy(std::string_view s);
WeComGroupPolicy parse_wecom_group_policy(std::string_view s);
std::string_view to_string(WeComDmPolicy p);
std::string_view to_string(WeComGroupPolicy p);

struct WeComGroupRule {
    WeComGroupPolicy policy = WeComGroupPolicy::Open;
    std::vector<std::string> allow_from;
};

// ---------------------------------------------------------------------------
// Upload session state.
// ---------------------------------------------------------------------------

struct WeComUploadSession {
    std::string media_type;     // image|file|voice|video
    std::string filename;
    std::string content_type;
    std::size_t total_size = 0;
    std::string media_key;      // server-assigned token
    std::size_t chunk_index = 0;
    std::size_t sent_bytes = 0;
    bool complete = false;
};

// ---------------------------------------------------------------------------
// Adapter.
// ---------------------------------------------------------------------------

class WeComAdapter : public BasePlatformAdapter {
public:
    struct Config {
        // Credentials.
        std::string bot_id;
        std::string secret;
        std::string message_token;  // legacy compat
        std::string websocket_url = kWeComDefaultWs;

        // Corporate REST fallback.
        std::string corp_id;
        std::string corp_secret;
        std::string agent_id;
        std::string webhook_url;  // group robot

        // DM policy.
        std::string dm_policy = "open";
        std::vector<std::string> allow_from;

        // Group policy.
        std::string group_policy = "open";
        std::vector<std::string> group_allow_from;
        std::unordered_map<std::string, WeComGroupRule> groups;

        // Misc.
        std::string bot_name = "hermes";
        bool require_group_mention = true;
        std::size_t max_message_length = kWeComMaxMessageLength;
        int max_send_retries = 3;
        double text_batch_delay_sec = 0.6;
        std::size_t text_batch_max_messages = 8;
    };

    explicit WeComAdapter(Config cfg);
    WeComAdapter(Config cfg, hermes::llm::HttpTransport* transport);
    ~WeComAdapter() override = default;

    // ----- BasePlatformAdapter ---------------------------------------------
    Platform platform() const override { return Platform::WeCom; }
    bool connect() override;
    void disconnect() override;
    bool send(const std::string& chat_id, const std::string& content) override;
    void send_typing(const std::string& chat_id) override;
    AdapterErrorKind last_error_kind() const override { return last_error_kind_; }

    // ----- Corporate REST helpers (access_token rotation + messages) -------
    bool refresh_access_token();
    std::string access_token() const { return access_token_; }
    bool send_webhook_message(const std::string& content,
                              const std::string& msgtype = "text");
    bool send_markdown_via_api(const std::string& chat_id,
                               const std::string& markdown);
    bool send_textcard_via_api(const std::string& chat_id,
                               const std::string& title,
                               const std::string& description,
                               const std::string& url);
    bool send_template_card_via_api(const std::string& chat_id,
                                    const nlohmann::json& card);

    // ----- WebSocket-level API ---------------------------------------------
    // Build the initial subscribe payload (bot_id + signed timestamp).
    nlohmann::json build_subscribe_payload() const;

    // Build a send-message payload (native text + chat routing fields).
    nlohmann::json build_send_payload(const std::string& chat_id,
                                      const std::string& content,
                                      const std::string& msgtype = "text") const;

    // Build a heartbeat ping payload with a deterministic req_id.
    nlohmann::json build_ping_payload();

    // Build a response envelope (for replying to an inbound request).
    nlohmann::json build_response_payload(const std::string& req_id,
                                          const std::string& payload);

    // Correlate a pending request.  ``on_response`` is invoked with the
    // response body exactly once; expired pending requests are swept.
    using ResponseCallback =
        std::function<void(const nlohmann::json& body, bool ok)>;
    std::string register_pending(const std::string& cmd,
                                 ResponseCallback cb,
                                 double timeout_seconds = kWeComRequestTimeoutSec);
    bool complete_pending(const std::string& req_id,
                          const nlohmann::json& body, bool ok);
    std::size_t sweep_expired_pending();

    // Dispatch a decoded payload — invokes the registered response
    // callback for request-replies, or returns a MessageEvent when the
    // payload is a callback.  On event callbacks the event body is
    // written to `*out_event` and the function returns std::nullopt.
    std::optional<MessageEvent> dispatch_payload(const nlohmann::json& payload,
                                                 nlohmann::json* out_event = nullptr);

    // ----- Inbound event helpers -------------------------------------------
    static std::pair<std::string, std::string> extract_text_body(
        const nlohmann::json& body);
    static std::string extract_reply_req_id(const nlohmann::json& body);
    static std::string classify_event(const nlohmann::json& payload);
    std::vector<std::string> extract_media_refs(const nlohmann::json& body) const;

    // ----- Outbound builders ------------------------------------------------
    // Build a chunk-upload payload for the given session + slice of bytes.
    nlohmann::json build_upload_init(const WeComUploadSession& s);
    nlohmann::json build_upload_chunk(const WeComUploadSession& s,
                                      const std::string& chunk_bytes);
    nlohmann::json build_upload_finish(const WeComUploadSession& s);

    // Plan a chunked upload of ``bytes`` — returns the sequence of
    // per-chunk payloads that the runner should send in order.
    std::vector<nlohmann::json> plan_upload_payloads(const WeComUploadSession& s,
                                                     const std::string& bytes);

    // ----- Policy gating ----------------------------------------------------
    bool is_dm_allowed(const std::string& sender_id) const;
    bool is_group_allowed(const std::string& chat_id,
                          const std::string& sender_id,
                          const std::string& text) const;
    WeComGroupRule resolve_group_rule(const std::string& chat_id) const;

    // ----- Dedup ------------------------------------------------------------
    WeComDedupCache& dedup_cache() { return dedup_; }

    // ----- Accessors --------------------------------------------------------
    const Config& config() const { return cfg_; }
    bool connected() const { return connected_; }
    static std::string new_req_id(const std::string& prefix = "req");

private:
    hermes::llm::HttpTransport* get_transport();

    Config cfg_;
    hermes::llm::HttpTransport* transport_ = nullptr;
    std::atomic<bool> connected_{false};
    AdapterErrorKind last_error_kind_ = AdapterErrorKind::None;
    std::string last_error_;

    // Corporate REST state.
    std::string access_token_;
    std::chrono::steady_clock::time_point access_token_expiry_{};

    WeComDedupCache dedup_;

    // Pending WebSocket request map.
    struct Pending {
        std::string cmd;
        ResponseCallback cb;
        std::chrono::steady_clock::time_point expires;
    };
    mutable std::mutex pending_mu_;
    std::unordered_map<std::string, Pending> pending_;

    // Reply-req-id for inbound messages (used to correlate a reply back
    // into the original request).
    mutable std::mutex reply_mu_;
    std::unordered_map<std::string, std::string> reply_req_ids_;
};

}  // namespace hermes::gateway::platforms
