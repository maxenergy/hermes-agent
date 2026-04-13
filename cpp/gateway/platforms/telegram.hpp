// Phase 12 — Telegram platform adapter (full depth port of
// gateway/platforms/telegram.py).
#pragma once

#include <chrono>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include <hermes/gateway/gateway_runner.hpp>
#include <hermes/llm/llm_client.hpp>

namespace hermes::gateway::platforms {

enum class TelegramChatType {
    Unknown,
    Private,
    Group,
    Supergroup,
    Channel,
};

TelegramChatType parse_chat_type(const std::string& raw);
std::string chat_type_to_string(TelegramChatType t);

struct InlineKeyboardButton {
    std::string text;
    std::optional<std::string> callback_data;
    std::optional<std::string> url;
    std::optional<std::string> switch_inline_query;
    std::optional<std::string> switch_inline_query_current_chat;
    static InlineKeyboardButton data(std::string text, std::string cb) {
        InlineKeyboardButton b;
        b.text = std::move(text);
        b.callback_data = std::move(cb);
        return b;
    }
    static InlineKeyboardButton link(std::string text, std::string u) {
        InlineKeyboardButton b;
        b.text = std::move(text);
        b.url = std::move(u);
        return b;
    }
    nlohmann::json to_json() const;
};

using InlineKeyboardRow = std::vector<InlineKeyboardButton>;

struct InlineKeyboardMarkup {
    std::vector<InlineKeyboardRow> rows;
    nlohmann::json to_json() const;
};

struct ReplyKeyboardButton {
    std::string text;
    bool request_contact = false;
    bool request_location = false;
    nlohmann::json to_json() const;
};

struct ReplyKeyboardMarkup {
    std::vector<std::vector<ReplyKeyboardButton>> rows;
    bool resize_keyboard = true;
    bool one_time_keyboard = false;
    bool selective = false;
    std::optional<std::string> input_field_placeholder;
    nlohmann::json to_json() const;
};

enum class TelegramErrorKind {
    None,
    Transient,
    FloodWait,
    BadRequest,
    ThreadNotFound,
    ReplyNotFound,
    MessageTooLong,
    NotModified,
    Unauthorized,
    Forbidden,
    ChatMigrated,
    Fatal,
};

struct TelegramError {
    TelegramErrorKind kind = TelegramErrorKind::None;
    int http_status = 0;
    std::string description;
    double retry_after_seconds = 0.0;
    std::optional<long long> migrate_to_chat_id;
};

TelegramError classify_telegram_error(int http_status,
                                      const std::string& response_body);

class MediaGroupBuffer {
public:
    struct Entry {
        std::chrono::steady_clock::time_point first_seen;
        std::vector<nlohmann::json> messages;
    };
    bool append(const nlohmann::json& message);
    std::vector<std::vector<nlohmann::json>> drain_expired(
        std::chrono::milliseconds max_age);
    std::optional<std::vector<nlohmann::json>> drain(const std::string& id);
    std::size_t pending() const { return buffer_.size(); }

private:
    std::unordered_map<std::string, Entry> buffer_;
};

std::vector<std::string> split_message_for_telegram(
    const std::string& text, std::size_t max_len = 4096);

std::string strip_markdown_v2(const std::string& text);
std::string format_message_markdown_v2(const std::string& content);

// SendOptions is declared at namespace scope so that it is fully defined
// when used as a defaulted argument inside TelegramAdapter.
struct TelegramSendOptions {
    std::optional<long long> reply_to_message_id;
    std::optional<long long> message_thread_id;
    std::optional<InlineKeyboardMarkup> inline_keyboard;
    std::optional<ReplyKeyboardMarkup> reply_keyboard;
    bool remove_reply_keyboard = false;
    bool disable_web_page_preview = false;
    bool disable_notification = false;
    bool protect_content = false;
    std::string parse_mode = "MarkdownV2";
};

class TelegramAdapter : public BasePlatformAdapter {
public:
    struct Config {
        std::string bot_token;
        bool use_webhook = false;
        std::string webhook_url;
        int webhook_port = 8443;
        std::string webhook_secret;
        std::string reply_to_mode = "first";
        std::vector<std::string> fallback_ips;
        std::string base_url;
        std::string base_file_url;
        bool require_mention = false;
        std::unordered_set<std::string> free_response_chats;
        std::vector<std::string> mention_patterns;
        bool reactions_enabled = false;
        std::unordered_set<std::string> allowed_user_ids;
        int getupdates_timeout_s = 30;
        int getupdates_limit = 100;
        bool drop_pending_updates = true;
        int max_send_retries = 3;
        double max_flood_wait_s = 5.0;
        std::string media_cache_dir;
    };

    using SendOptions = TelegramSendOptions;

    struct SendResult {
        bool ok = false;
        std::vector<long long> message_ids;
        TelegramError last_error;
    };

    static constexpr std::size_t kMaxMessageLength = 4096;
    static constexpr std::size_t kSplitThreshold = 4000;
    static constexpr std::size_t kCaptionMaxLength = 1024;
    static constexpr std::chrono::milliseconds kMediaGroupWait{800};

    explicit TelegramAdapter(Config cfg);
    TelegramAdapter(Config cfg, hermes::llm::HttpTransport* transport);
    ~TelegramAdapter() override;

    Platform platform() const override { return Platform::Telegram; }
    bool connect() override;
    void disconnect() override;
    bool send(const std::string& chat_id, const std::string& content) override;
    void send_typing(const std::string& chat_id) override;

    SendResult send_message(const std::string& chat_id,
                            const std::string& content,
                            const SendOptions& opts = TelegramSendOptions{});

    SendResult edit_message_text(const std::string& chat_id,
                                 long long message_id,
                                 const std::string& content,
                                 const std::string& parse_mode = "MarkdownV2");

    bool delete_message(const std::string& chat_id, long long message_id);
    SendResult forward_message(const std::string& from_chat_id,
                               const std::string& to_chat_id,
                               long long message_id);
    SendResult copy_message(const std::string& from_chat_id,
                            const std::string& to_chat_id,
                            long long message_id,
                            const std::optional<std::string>& caption = std::nullopt);

    bool pin_chat_message(const std::string& chat_id, long long message_id,
                          bool disable_notification = true);
    bool unpin_chat_message(const std::string& chat_id,
                            std::optional<long long> message_id = std::nullopt);

    bool set_reaction(const std::string& chat_id, long long message_id,
                      const std::string& emoji);
    bool clear_reactions(const std::string& chat_id, long long message_id);

    bool set_my_commands(
        const std::vector<std::pair<std::string, std::string>>& commands);
    bool delete_my_commands();
    bool set_chat_menu_button(const std::string& chat_id,
                              const std::string& button_type = "commands",
                              const std::optional<std::string>& text = std::nullopt,
                              const std::optional<std::string>& web_app_url = std::nullopt);

    nlohmann::json get_me();
    nlohmann::json get_chat(const std::string& chat_id);
    nlohmann::json get_chat_member(const std::string& chat_id,
                                   const std::string& user_id);
    nlohmann::json get_user_profile_photos(const std::string& user_id, int limit = 1);

    std::optional<std::string> get_file_download_url(const std::string& file_id);
    std::string download_file(const std::string& file_id);

    SendResult send_photo(const std::string& chat_id,
                          const std::string& photo,
                          const std::optional<std::string>& caption = std::nullopt,
                          const SendOptions& opts = TelegramSendOptions{});
    SendResult send_document(const std::string& chat_id,
                             const std::string& document,
                             const std::optional<std::string>& filename = std::nullopt,
                             const std::optional<std::string>& caption = std::nullopt,
                             const SendOptions& opts = TelegramSendOptions{});
    SendResult send_video(const std::string& chat_id,
                          const std::string& video,
                          const std::optional<std::string>& caption = std::nullopt,
                          const SendOptions& opts = TelegramSendOptions{});
    SendResult send_audio(const std::string& chat_id,
                          const std::string& audio,
                          const std::optional<std::string>& caption = std::nullopt,
                          const SendOptions& opts = TelegramSendOptions{});
    SendResult send_voice(const std::string& chat_id,
                          const std::string& voice,
                          const std::optional<std::string>& caption = std::nullopt,
                          const SendOptions& opts = TelegramSendOptions{});
    SendResult send_animation(const std::string& chat_id,
                              const std::string& animation,
                              const std::optional<std::string>& caption = std::nullopt,
                              const SendOptions& opts = TelegramSendOptions{});
    SendResult send_sticker(const std::string& chat_id,
                            const std::string& sticker,
                            const SendOptions& opts = TelegramSendOptions{});

    SendResult send_poll(const std::string& chat_id,
                         const std::string& question,
                         const std::vector<std::string>& options,
                         bool is_anonymous = true, bool is_quiz = false,
                         std::optional<int> correct_option_id = std::nullopt);

    SendResult send_location(const std::string& chat_id, double latitude,
                             double longitude,
                             const SendOptions& opts = TelegramSendOptions{});

    bool answer_callback_query(const std::string& callback_query_id,
                               const std::optional<std::string>& text = std::nullopt,
                               bool show_alert = false,
                               std::optional<int> cache_time = std::nullopt);

    bool set_webhook(const std::string& url,
                     const std::optional<std::string>& secret_token = std::nullopt,
                     const std::vector<std::string>& allowed_updates = {});
    bool delete_webhook(bool drop_pending_updates = false);
    nlohmann::json get_webhook_info();
    nlohmann::json get_updates();

    long long next_update_offset() const { return next_update_offset_; }
    void set_next_update_offset(long long v) { next_update_offset_ = v; }

    static std::optional<long long> parse_forum_topic(const nlohmann::json& message);
    static std::optional<std::string> parse_media_group_id(const nlohmann::json& message);
    static TelegramChatType parse_message_chat_type(const nlohmann::json& message);
    static bool is_forum_topic_created(const nlohmann::json& message);
    static bool is_forum_topic_closed(const nlohmann::json& message);
    static std::optional<long long> parse_reply_to_message_id(const nlohmann::json& message);
    static std::optional<long long> parse_migrate_to_chat_id(const nlohmann::json& message);
    static std::string parse_update_kind(const nlohmann::json& update);
    static bool message_mentions_bot(const nlohmann::json& message,
                                     const std::string& bot_username,
                                     std::optional<long long> bot_id = std::nullopt);
    static std::string clean_bot_trigger_text(const std::string& text,
                                              const std::string& bot_username);
    static std::string format_markdown_v2(const std::string& text);
    static std::string format_message(const std::string& content) {
        return format_message_markdown_v2(content);
    }
    bool should_process_message(const nlohmann::json& message,
                                bool is_command = false) const;
    static std::string classify_callback_data(const std::string& data);
    static std::string render_location_prompt(const nlohmann::json& message);

    Config config() const { return cfg_; }
    bool connected() const { return connected_; }
    const std::string& bot_username() const { return bot_username_; }
    void set_bot_username(std::string u) { bot_username_ = std::move(u); }
    long long bot_id() const { return bot_id_; }
    void set_bot_id(long long v) { bot_id_ = v; }

    MediaGroupBuffer& media_group_buffer() { return media_groups_; }

    std::chrono::steady_clock::time_point flood_wait_until() const {
        return flood_wait_until_;
    }
    void set_flood_wait(std::chrono::milliseconds dur) {
        flood_wait_until_ = std::chrono::steady_clock::now() + dur;
    }
    bool is_flood_waiting() const {
        return std::chrono::steady_clock::now() < flood_wait_until_;
    }

    long long register_approval(std::string session_key);
    std::optional<std::string> take_approval(long long approval_id);

    hermes::llm::HttpTransport* transport() { return get_transport(); }

private:
    hermes::llm::HttpTransport* get_transport();
    std::string api_url(const std::string& method) const;
    std::string file_api_url(const std::string& file_path) const;

    std::optional<nlohmann::json> call_api(
        const std::string& method, const nlohmann::json& payload,
        TelegramError* out_error = nullptr);

    struct ChunkResult {
        bool ok = false;
        long long message_id = 0;
        TelegramError error;
    };
    ChunkResult send_chunk(const std::string& chat_id,
                           const std::string& chunk,
                           const SendOptions& opts,
                           std::optional<long long> reply_to_override,
                           std::optional<long long> thread_override);

    Config cfg_;
    hermes::llm::HttpTransport* transport_ = nullptr;
    bool connected_ = false;
    std::string bot_username_;
    long long bot_id_ = 0;
    long long next_update_offset_ = 0;
    std::chrono::steady_clock::time_point flood_wait_until_{};

    MediaGroupBuffer media_groups_;

    mutable std::mutex approval_mu_;
    long long approval_counter_ = 0;
    std::unordered_map<long long, std::string> approval_state_;

    std::unordered_map<std::string, std::string> migrated_chats_;
    mutable std::mutex migrated_mu_;
};

}  // namespace hermes::gateway::platforms
