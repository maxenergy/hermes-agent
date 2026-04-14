// Phase 12 — Weixin (WeChat personal iLink) platform adapter.
//
// Depth port of gateway/platforms/weixin.py (1669 LoC).
// Covers:
//   * iLink long-poll session (getUpdates / sendMessage).
//   * QR-code login state machine (qrcode / check / wait4scan / wait4confirm).
//   * AES-128-ECB + PKCS#7 helpers (CDN upload/download crypto).
//   * Account + sync-buf persistence (~/.hermes/weixin/accounts).
//   * Context-token store + typing-ticket cache.
//   * Inbound item parsing (text / image / video / file / voice / link /
//     location / sticker / system).
//   * Group @mention gating; DM allowlist.
//   * Outbound text splitting + markdown normalization (table rewrite,
//     header rewrite, fence segmentation, 4500-char chunking).
//   * Forward / quote / recall / friend-request primitives.
//   * Media outbound builders (image / voice / video / file).
//
// All blocking network I/O is funnelled through hermes::llm::HttpTransport so
// unit tests can mock with FakeHttpTransport.
#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <deque>
#include <filesystem>
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
// Protocol constants (mirror the Python module-level constants).
// ---------------------------------------------------------------------------

inline constexpr const char* kILinkAppId = "ilink-bot";
inline constexpr int kILinkAppClientVersion = 1;
inline constexpr const char* kChannelVersion = "cpp/0.1";

inline constexpr const char* kEpGetUpdates = "getupdates";
inline constexpr const char* kEpSendMessage = "sendmessage";
inline constexpr const char* kEpSendTyping = "sendtypingstatus";
inline constexpr const char* kEpGetConfig = "getconfig";
inline constexpr const char* kEpGetUploadUrl = "getuploadurl";
inline constexpr const char* kEpLoginQrCode = "login/qrcode";
inline constexpr const char* kEpLoginCheck = "login/check";

inline constexpr int kMsgTypeBot = 1;
inline constexpr int kMsgStateFinish = 2;

inline constexpr const char* kItemText = "text";
inline constexpr const char* kItemImage = "image";
inline constexpr const char* kItemVideo = "video";
inline constexpr const char* kItemFile = "file";
inline constexpr const char* kItemVoice = "voice";
inline constexpr const char* kItemLink = "link";
inline constexpr const char* kItemLocation = "location";
inline constexpr const char* kItemSticker = "sticker";
inline constexpr const char* kItemSystem = "system";
inline constexpr const char* kItemQuote = "quote";

inline constexpr std::size_t kWeixinMaxChunkChars = 4500;

// ---------------------------------------------------------------------------
// Free-function helpers (module-level in Python).
// ---------------------------------------------------------------------------

// "abcdef01234" -> "abcdef01" (first `keep` chars).  Returns "?" on empty.
std::string weixin_safe_id(const std::string& value, std::size_t keep = 8);

// Canonical JSON dump (no spaces, UTF-8 pass-through).
std::string weixin_json_dumps(const nlohmann::json& payload);

// Byte helpers.
std::string weixin_pkcs7_pad(const std::string& data, std::size_t block = 16);
std::string weixin_pkcs7_unpad(const std::string& data, std::size_t block = 16);

// AES-128-ECB encrypt/decrypt (used by the CDN upload/download helpers).
// Returns empty string on error (e.g., missing OpenSSL).
std::string weixin_aes128_ecb_encrypt(const std::string& plaintext,
                                      const std::string& key);
std::string weixin_aes128_ecb_decrypt(const std::string& ciphertext,
                                      const std::string& key);

std::size_t weixin_aes_padded_size(std::size_t n);

// Random base64-encoded UIN (used in every iLink request header).
std::string weixin_random_uin();

// Build iLink request headers (Content-Length computed from body).
std::unordered_map<std::string, std::string> weixin_headers(
    const std::optional<std::string>& token, const std::string& body);

// Parse a base64 AES key — handles both the 16-byte and hex-inside-32-byte
// variants that Weixin servers emit.  Returns empty on malformed input.
std::string weixin_parse_aes_key(const std::string& aes_key_b64);

// Percent-encode for CDN URL query parameter usage (safe="").
std::string weixin_url_quote(std::string_view value);

std::string weixin_cdn_download_url(const std::string& cdn_base_url,
                                    const std::string& encrypted_query_param);
std::string weixin_cdn_upload_url(const std::string& cdn_base_url,
                                  const std::string& upload_param,
                                  const std::string& filekey);

// Sniff the chat kind (DM vs group) from an inbound message dict.
struct WeixinChatKind {
    std::string kind;   // "dm" or "group"
    std::string chat_id;
};
WeixinChatKind weixin_guess_chat_type(const nlohmann::json& message,
                                      const std::string& account_id);

// Markdown-delivery helpers (ported from the ``_..._for_weixin`` family).
std::string weixin_rewrite_headers(const std::string& line);
std::string weixin_rewrite_table_block(const std::vector<std::string>& lines);
std::string weixin_normalize_markdown_blocks(const std::string& content);
std::vector<std::string> weixin_split_markdown_blocks(const std::string& content);
std::vector<std::string> weixin_pack_markdown_blocks(const std::string& content,
                                                     std::size_t max_length);
std::vector<std::string> weixin_split_text_for_delivery(
    const std::string& content, std::size_t max_length = kWeixinMaxChunkChars);

// Concatenate text items from an item_list array.
std::string weixin_extract_text(const nlohmann::json& item_list);

// Mime type from filename extension (tiny lookup table — matches Python's).
std::string weixin_mime_from_filename(const std::string& filename);

// Table-row splitter (markdown "| a | b |" -> ["a", "b"]).
std::vector<std::string> weixin_split_table_row(const std::string& line);

// XML tag extractor (used for official-account callback messages).
std::string weixin_extract_xml_tag(const std::string& xml,
                                   const std::string& tag);

// Map media_types + text to a MessageEvent message_type string.
std::string weixin_message_type_from_media(
    const std::vector<std::string>& media_types, const std::string& text);

// ---------------------------------------------------------------------------
// Account persistence (JSON files under $HERMES_HOME/weixin/accounts).
// ---------------------------------------------------------------------------

struct WeixinAccountRecord {
    std::string account_id;
    std::string token;
    std::string base_url;
    std::string user_id;
    std::string saved_at;  // ISO-8601 UTC timestamp
};

std::filesystem::path weixin_account_dir(const std::string& hermes_home);
std::filesystem::path weixin_account_file(const std::string& hermes_home,
                                          const std::string& account_id);

void weixin_save_account(const std::string& hermes_home,
                         const WeixinAccountRecord& rec);
std::optional<WeixinAccountRecord> weixin_load_account(
    const std::string& hermes_home, const std::string& account_id);

// Sync-buffer persistence (one-line blob per account).
std::filesystem::path weixin_sync_buf_path(const std::string& hermes_home,
                                           const std::string& account_id);
std::string weixin_load_sync_buf(const std::string& hermes_home,
                                 const std::string& account_id);
void weixin_save_sync_buf(const std::string& hermes_home,
                          const std::string& account_id,
                          const std::string& sync_buf);

// ---------------------------------------------------------------------------
// In-memory caches — ContextTokenStore + TypingTicketCache.
// ---------------------------------------------------------------------------

// Disk-backed context_token cache keyed by (account_id, user_id).
class WeixinContextTokenStore {
public:
    explicit WeixinContextTokenStore(std::string hermes_home);

    // Restore cache entries from disk for one account.  Silently ignores a
    // missing or malformed file.
    void restore(const std::string& account_id);

    std::optional<std::string> get(const std::string& account_id,
                                   const std::string& user_id) const;
    void set(const std::string& account_id, const std::string& user_id,
             const std::string& token);
    std::size_t size() const;

private:
    std::string key(const std::string& account_id,
                    const std::string& user_id) const {
        return account_id + ":" + user_id;
    }
    std::filesystem::path path_for(const std::string& account_id) const;
    void persist(const std::string& account_id);

    std::string hermes_home_;
    mutable std::mutex mu_;
    std::unordered_map<std::string, std::string> cache_;
};

// TTL-expiring typing-ticket cache (values live ~10 minutes by default).
class WeixinTypingTicketCache {
public:
    explicit WeixinTypingTicketCache(double ttl_seconds = 600.0)
        : ttl_seconds_(ttl_seconds) {}

    std::optional<std::string> get(const std::string& user_id);
    void set(const std::string& user_id, const std::string& ticket);
    std::size_t size() const;

private:
    struct Entry {
        std::string ticket;
        std::chrono::steady_clock::time_point stored;
    };
    double ttl_seconds_;
    mutable std::mutex mu_;
    std::unordered_map<std::string, Entry> cache_;
};

// ---------------------------------------------------------------------------
// Structured XML message (official-account callback encoding).
// ---------------------------------------------------------------------------

struct WeixinMessageEvent {
    std::string from_user;
    std::string to_user;
    std::string msg_type;
    std::string content;
    std::string msg_id;
    std::string event;          // subscribe / unsubscribe / ...
    std::string event_key;
    std::string pic_url;
    std::string media_id;
    std::string format;
    std::string location_x;
    std::string location_y;
    std::string title;
    std::string url;
};

// ---------------------------------------------------------------------------
// QR-code login state machine.
// ---------------------------------------------------------------------------

enum class WeixinQrPhase {
    New,            // qrcode requested, not yet displayed
    WaitScan,
    WaitConfirm,
    LoggedIn,
    Cancelled,
    Timeout,
    Error,
};

struct WeixinQrStatus {
    WeixinQrPhase phase = WeixinQrPhase::New;
    std::string qr_url;
    std::string token;
    std::string base_url;
    std::string user_id;
    std::string message;    // error / informational
};

std::string_view to_string(WeixinQrPhase p);
WeixinQrPhase parse_qr_phase(std::string_view s);

// ---------------------------------------------------------------------------
// The adapter proper.
// ---------------------------------------------------------------------------

class WeixinAdapter : public BasePlatformAdapter {
public:
    struct Config {
        // Official-account subscription credentials.
        std::string appid;
        std::string appsecret;
        std::string token;          // verification token

        // iLink personal-WeChat credentials.
        std::string account_id;
        std::string base_url;
        std::string user_token;     // long-lived, persisted per account
        std::string user_id;
        std::string hermes_home;    // for persistence

        // DM + group policy.
        std::vector<std::string> allowed_dm_senders;
        std::vector<std::string> allowed_groups;
        std::vector<std::string> admin_user_ids;
        bool require_group_mention = true;
        std::string bot_name = "hermes";

        // Delivery tuning.
        std::size_t max_chunk_chars = kWeixinMaxChunkChars;
        int poll_timeout_ms = 25000;
        int api_timeout_ms = 15000;
        int send_max_retries = 3;
    };

    explicit WeixinAdapter(Config cfg);
    WeixinAdapter(Config cfg, hermes::llm::HttpTransport* transport);
    ~WeixinAdapter() override = default;

    // ----- BasePlatformAdapter ---------------------------------------------
    Platform platform() const override { return Platform::Weixin; }
    bool connect() override;
    void disconnect() override;
    bool send(const std::string& chat_id, const std::string& content) override;
    void send_typing(const std::string& chat_id) override;
    AdapterErrorKind last_error_kind() const override {
        return last_error_kind_;
    }

    // ----- Official-account API (legacy appid/appsecret flow) --------------
    // Acquire the cgi-bin access_token, populates wx_access_token_.
    bool refresh_access_token();
    bool send_custom_text(const std::string& touser, const std::string& text);
    bool send_custom_typing(const std::string& touser);
    static WeixinMessageEvent parse_xml_message(const std::string& xml);
    std::string wx_access_token() const { return wx_access_token_; }

    // ----- iLink personal-WeChat flow ---------------------------------------
    // QR-code login phases.  call_qr_login() + poll_qr_status() cooperate to
    // drive the state machine end-to-end.
    WeixinQrStatus request_qr_code();
    WeixinQrStatus poll_qr_status(const std::string& token);

    // One iteration of the long-poll loop — fetches messages and parses them.
    struct PollResult {
        bool ok = false;
        std::vector<nlohmann::json> messages;
        std::string next_sync_buf;
        std::string error;
    };
    PollResult poll_once();

    // Send a text message via iLink (personal WeChat).
    bool ilink_send_text(const std::string& to_user,
                         const std::string& text,
                         const std::optional<std::string>& context_token =
                             std::nullopt);

    // Fetch typing ticket + send typing indicator.
    bool ilink_fetch_config(std::string* typing_ticket_out,
                            std::string* cdn_base_url_out);
    bool ilink_send_typing(const std::string& user_id, bool is_typing = true);

    // CDN upload/download helpers.
    struct UploadUrlInfo {
        std::string upload_url;
        std::string encrypted_param;
        std::string filekey;
        std::string aes_key;
    };
    std::optional<UploadUrlInfo> ilink_get_upload_url(
        const std::string& media_type, const std::string& filename,
        std::size_t file_size);
    bool ilink_upload_ciphertext(const UploadUrlInfo& info,
                                 const std::string& ciphertext);
    std::string ilink_download_bytes(const std::string& encrypted_param,
                                     const std::string& aes_key,
                                     const std::string& cdn_base_url);

    // ----- Message-level helpers --------------------------------------------
    // Decide if a sender is allowed in DM.
    bool is_dm_allowed(const std::string& sender_id) const;
    // Group policy — sender must be admin or message must mention bot.
    bool is_group_allowed(const std::string& chat_id,
                          const std::string& sender_id,
                          const std::string& text) const;

    // Convert an inbound iLink payload into a MessageEvent.
    std::optional<MessageEvent> message_from_payload(
        const nlohmann::json& payload);

    // Outbound markdown/text chunker honouring kWeixinMaxChunkChars.
    std::vector<std::string> split_text(const std::string& content) const;

    // Send a media message.  The builder is selected by filename extension.
    bool send_image(const std::string& chat_id,
                    const std::string& image_path_or_url);
    bool send_video(const std::string& chat_id, const std::string& video_path);
    bool send_voice(const std::string& chat_id, const std::string& voice_path);
    bool send_document(const std::string& chat_id, const std::string& file_path,
                       const std::string& caption = {});

    // Forward / quote / recall primitives.
    bool forward_message(const std::string& chat_id,
                         const std::string& source_msg_id);
    bool quote_message(const std::string& chat_id,
                       const std::string& quoted_msg_id,
                       const std::string& reply_text);
    bool recall_message(const std::string& chat_id, const std::string& msg_id);
    bool send_friend_request(const std::string& user_id,
                             const std::string& greeting);
    bool accept_friend_request(const std::string& request_id);

    // ----- Accessors --------------------------------------------------------
    const Config& config() const { return cfg_; }
    bool connected() const { return connected_; }
    const std::string& sync_buf() const { return sync_buf_; }
    WeixinContextTokenStore& context_token_store() { return ctx_tokens_; }
    WeixinTypingTicketCache& typing_ticket_cache() { return typing_tickets_; }

    // Helper: build a unique client_id for a new outbound message (UUID-ish).
    static std::string new_client_id();

private:
    hermes::llm::HttpTransport* get_transport();
    nlohmann::json ilink_api_post(const std::string& endpoint,
                                  const nlohmann::json& payload);
    nlohmann::json ilink_api_get(const std::string& endpoint);

    Config cfg_;
    hermes::llm::HttpTransport* transport_ = nullptr;
    bool connected_ = false;
    AdapterErrorKind last_error_kind_ = AdapterErrorKind::None;
    std::string last_error_;

    // Official-account state.
    std::string wx_access_token_;
    std::chrono::steady_clock::time_point wx_token_expiry_{};

    // iLink state.
    std::string sync_buf_;
    std::string cdn_base_url_;

    WeixinContextTokenStore ctx_tokens_;
    WeixinTypingTicketCache typing_tickets_;

    // Dedup cache for inbound msg_id.
    mutable std::mutex seen_mu_;
    std::unordered_set<std::string> seen_msg_ids_;
    std::deque<std::string> seen_order_;
    std::size_t seen_cap_ = 4096;
};

}  // namespace hermes::gateway::platforms
