// Phase 12 — Discord platform adapter (depth parity with Python).
// Phase 14 — voice via libopus. Phase 18 — full REST surface.
#pragma once

#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <hermes/gateway/gateway_runner.hpp>
#include <hermes/llm/llm_client.hpp>
#include <nlohmann/json.hpp>

#include "discord_gateway.hpp"
#include "opus_codec.hpp"

namespace hermes::gateway::platforms {

// ─── Embed Builder ────────────────────────────────────────────────────────
//
// Discord embeds have many optional fields. This builder mirrors the subset
// used in the Python adapter's `discord.Embed(...)` construction and emits
// the canonical JSON that Discord's REST API expects. Discord limits:
//   - title ≤ 256, description ≤ 4096, fields ≤ 25, field.name ≤ 256,
//   - field.value ≤ 1024, footer.text ≤ 2048, author.name ≤ 256.
// The builder silently truncates to these caps so callers never trip
// error code 50035 ("Invalid Form Body").
class DiscordEmbed {
public:
    struct Field {
        std::string name;
        std::string value;
        bool inline_ = false;
    };

    DiscordEmbed& set_title(std::string t);
    DiscordEmbed& set_description(std::string d);
    DiscordEmbed& set_url(std::string u);
    DiscordEmbed& set_color(uint32_t c);
    DiscordEmbed& set_timestamp_iso8601(std::string ts);
    DiscordEmbed& set_footer(std::string text,
                             std::string icon_url = "");
    DiscordEmbed& set_thumbnail(std::string url);
    DiscordEmbed& set_image(std::string url);
    DiscordEmbed& set_author(std::string name,
                             std::string url = "",
                             std::string icon_url = "");
    DiscordEmbed& add_field(std::string name, std::string value,
                            bool inline_val = false);

    nlohmann::json to_json() const;

    // Named color palette matching `discord.Color.*` helpers.
    static constexpr uint32_t kColorOrange = 0xE67E22;
    static constexpr uint32_t kColorGold = 0xF1C40F;
    static constexpr uint32_t kColorBlue = 0x3498DB;
    static constexpr uint32_t kColorGreen = 0x2ECC71;
    static constexpr uint32_t kColorRed = 0xE74C3C;
    static constexpr uint32_t kColorGrey = 0x95A5A6;

private:
    std::string title_;
    std::string description_;
    std::string url_;
    std::optional<uint32_t> color_;
    std::string timestamp_;
    std::string footer_text_;
    std::string footer_icon_url_;
    std::string thumbnail_url_;
    std::string image_url_;
    std::string author_name_;
    std::string author_url_;
    std::string author_icon_url_;
    std::vector<Field> fields_;
};

// ─── Slash Command Registry Entry ────────────────────────────────────────
struct SlashCommand {
    std::string name;           // "hermes"
    std::string description;    // "Interact with Hermes agent"
    int type = 1;               // CHAT_INPUT
    nlohmann::json options = nlohmann::json::array();
    std::vector<std::string> aliases;
};

// ─── Interaction (button / select / modal / autocomplete) ───────────────
struct DiscordInteraction {
    // interaction.type — 1 PING, 2 APPLICATION_COMMAND, 3 MESSAGE_COMPONENT,
    // 4 APPLICATION_COMMAND_AUTOCOMPLETE, 5 MODAL_SUBMIT.
    int type = 0;
    std::string id;
    std::string token;
    std::string application_id;
    std::string channel_id;
    std::string guild_id;
    std::string user_id;
    std::string username;
    std::string custom_id;           // component / modal
    int component_type = 0;          // 2=button, 3=select, 4=text-input
    std::vector<std::string> values; // select-menu values
    std::string command_name;        // slash command name
    nlohmann::json raw;              // original payload for advanced use
};

// ─── Rate-Limit Bucket ────────────────────────────────────────────────────
// Discord returns X-RateLimit-Bucket per route + X-RateLimit-Remaining and
// Retry-After on 429. This small structure tracks the *local* bucket state
// so the adapter can proactively sleep. It does NOT perform network I/O;
// the caller decides whether to block on `wait_for()` before a request.
struct RateLimitBucket {
    int remaining = 1;
    double reset_after_seconds = 0.0;
    std::chrono::steady_clock::time_point last_update =
        std::chrono::steady_clock::now();
    bool global = false;
};

// ─── Webhook Route ────────────────────────────────────────────────────────
// When a webhook URL is configured, outgoing messages bypass the bot rate
// limits and instead hit /webhooks/{id}/{token}. Useful for high-volume
// channels where the bot would otherwise exhaust its 5/channel bucket.
struct DiscordWebhook {
    std::string id;
    std::string token;
    std::string name;
    std::string avatar_url;
    std::string channel_id;
};

// ─── Attachment Spec ──────────────────────────────────────────────────────
// Describes a single file to upload via multipart/form-data. The adapter
// wraps this into the payload_json + files[n] parts the Discord API wants.
struct AttachmentSpec {
    std::string filename;
    std::string content_type;  // e.g. "audio/ogg"
    std::vector<uint8_t> data;
    // Optional voice-message metadata (Discord flag 8192).
    std::optional<double> duration_secs;
    std::vector<uint8_t> waveform;
};

// ─── Allowed Mentions ─────────────────────────────────────────────────────
// Matches the Discord "allowed_mentions" object. Defaults to {parse: []}
// which blocks all mentions — the safer default for an LLM agent that
// might echo `@everyone` from user input.
struct AllowedMentions {
    bool parse_users = false;
    bool parse_roles = false;
    bool parse_everyone = false;
    std::vector<std::string> users;
    std::vector<std::string> roles;
    bool replied_user = true;

    nlohmann::json to_json() const;
};

class DiscordAdapter : public BasePlatformAdapter {
public:
    struct Config {
        std::string bot_token;
        std::string application_id;
        std::string public_key;          // for interaction signature verify
        bool manage_threads = true;
        bool auto_register_commands = true;
        std::string guild_id;            // when non-empty, register guild-scoped
        bool intents_members = false;
        bool intents_voice_states = false;
        bool intents_message_content = true;
        // Reply-to policy: "first" (default), "all", "off".
        std::string reply_to_mode = "first";
        // Webhook fallback for outgoing text; empty disables.
        std::string webhook_url;
        // Sharding hint (ignored when <=1).
        int shard_id = 0;
        int shard_count = 1;
        // Max message length. Discord enforces 2000 for bots.
        int max_message_length = 2000;
        // Voice channel idle-timeout (seconds) before automatic disconnect.
        int voice_idle_timeout_s = 300;
    };

    // A decoded 20ms frame from a remote speaker in a voice channel.
    struct VoicePacket {
        uint32_t ssrc = 0;
        uint16_t sequence = 0;
        uint32_t timestamp = 0;
        std::vector<int16_t> pcm;
    };

    struct SsrcUserMapping {
        uint32_t ssrc;
        std::string user_id;
    };

    using VoiceCallback = std::function<void(const VoicePacket&)>;
    using InteractionCallback = std::function<void(const DiscordInteraction&)>;

    explicit DiscordAdapter(Config cfg);
    DiscordAdapter(Config cfg, hermes::llm::HttpTransport* transport);

    Platform platform() const override { return Platform::Discord; }
    bool connect() override;
    void disconnect() override;
    bool send(const std::string& chat_id, const std::string& content) override;
    void send_typing(const std::string& chat_id) override;

    // ─── Mention / formatting helpers ────────────────────────────────────
    static std::string format_mention(const std::string& user_id);
    static std::string format_channel_mention(const std::string& channel_id);
    static std::string format_role_mention(const std::string& role_id);
    // Parse `<@user>`, `<@!user>`, `<#channel>`, `<@&role>` out of content;
    // returns the IDs in the three vectors passed by reference.
    static void parse_mentions(const std::string& content,
                               std::vector<std::string>& users,
                               std::vector<std::string>& channels,
                               std::vector<std::string>& roles);
    // Strip the first occurrence of our bot mention from the front of a
    // message (mirrors discord.py's Message.clean_content trim behaviour).
    static std::string strip_leading_mention(const std::string& content,
                                             const std::string& bot_id);

    // ─── Message splitting ────────────────────────────────────────────────
    // Splits `content` into chunks ≤ `limit`, trying to keep triple-backtick
    // code blocks intact by re-opening/closing fences across boundaries.
    static std::vector<std::string> split_message(const std::string& content,
                                                  int limit = 2000);

    // ─── Slash commands ──────────────────────────────────────────────────
    // Register a single command definition. Subsequent bulk_register_commands
    // will push all registered ones to Discord in one PUT.
    void register_slash_command(SlashCommand cmd);
    // Push all registered commands to Discord via PUT
    // /applications/{app}/commands (global) or /guilds/{gid}/commands.
    bool bulk_register_commands();
    // Seed the standard Hermes slash-command set mirroring the Python port.
    void register_hermes_slash_commands();
    const std::vector<SlashCommand>& slash_commands() const {
        return slash_commands_;
    }

    // ─── Interactions ────────────────────────────────────────────────────
    void set_interaction_callback(InteractionCallback cb);
    // Respond to an interaction (type 4 = CHANNEL_MESSAGE_WITH_SOURCE).
    bool respond_interaction(const std::string& interaction_id,
                             const std::string& token,
                             const std::string& content,
                             int response_type = 4,
                             bool ephemeral = false);
    // Respond with an embed.
    bool respond_interaction_embed(const std::string& interaction_id,
                                   const std::string& token,
                                   const DiscordEmbed& embed,
                                   bool ephemeral = false);
    // Defer an interaction (type 5); good when work takes >3s.
    bool defer_interaction(const std::string& interaction_id,
                           const std::string& token,
                           bool ephemeral = false);
    // Follow-up message to a deferred interaction.
    bool followup_interaction(const std::string& token,
                              const std::string& content);
    // Autocomplete response — returns up to 25 choices for an option.
    bool respond_autocomplete(const std::string& interaction_id,
                              const std::string& token,
                              const std::vector<std::pair<std::string, std::string>>& choices);
    // Dispatch a raw INTERACTION_CREATE payload (from the gateway) to the
    // registered interaction callback after parsing.
    void dispatch_interaction_payload(const nlohmann::json& payload);

    // ─── Channels / Threads / Forums ─────────────────────────────────────
    // Thread type 11 (PUBLIC_THREAD), 12 (PRIVATE_THREAD), 10 (NEWS).
    bool create_thread(const std::string& channel_id,
                       const std::string& name,
                       int auto_archive_minutes = 1440);
    bool create_thread_from_message(const std::string& channel_id,
                                    const std::string& message_id,
                                    const std::string& name,
                                    int auto_archive_minutes = 1440);
    bool archive_thread(const std::string& thread_id);
    bool unarchive_thread(const std::string& thread_id);
    bool lock_thread(const std::string& thread_id);
    bool unlock_thread(const std::string& thread_id);
    bool delete_thread(const std::string& thread_id);
    // Forum-specific: create a forum post (POST /channels/{forum}/threads
    // with a message body).
    bool create_forum_post(const std::string& forum_channel_id,
                           const std::string& title,
                           const std::string& message_content,
                           const std::vector<std::string>& tag_ids = {});

    bool send_to_thread(const std::string& thread_id,
                        const std::string& content);
    // Reply to a specific message via message_reference.
    bool reply_to_message(const std::string& channel_id,
                          const std::string& message_id,
                          const std::string& content,
                          bool fail_if_not_exists = false);

    // ─── Messages ────────────────────────────────────────────────────────
    // Send content with embed and optional allowed_mentions.
    bool send_embed(const std::string& channel_id,
                    const DiscordEmbed& embed,
                    const std::string& content = "");
    // Send multiple embeds in one message (max 10).
    bool send_embeds(const std::string& channel_id,
                     const std::vector<DiscordEmbed>& embeds,
                     const std::string& content = "");
    // Send with custom allowed_mentions policy.
    bool send_with_policy(const std::string& channel_id,
                          const std::string& content,
                          const AllowedMentions& allowed);
    // Edit a message (content only).
    bool edit_message(const std::string& channel_id,
                      const std::string& message_id,
                      const std::string& new_content);
    // Edit a message with a new embed.
    bool edit_message_embed(const std::string& channel_id,
                            const std::string& message_id,
                            const DiscordEmbed& embed);
    // Delete a single message.
    bool delete_message(const std::string& channel_id,
                        const std::string& message_id);
    // Bulk delete (Discord accepts 2..100 IDs, all ≤14 days old).
    bool bulk_delete(const std::string& channel_id,
                     const std::vector<std::string>& message_ids);
    bool pin_message(const std::string& channel_id,
                     const std::string& message_id);
    bool unpin_message(const std::string& channel_id,
                       const std::string& message_id);
    bool fetch_message(const std::string& channel_id,
                       const std::string& message_id,
                       nlohmann::json& out);
    bool fetch_channel(const std::string& channel_id, nlohmann::json& out);
    bool fetch_guild(const std::string& guild_id, nlohmann::json& out);

    // ─── Reactions ────────────────────────────────────────────────────────
    bool add_reaction(const std::string& channel_id,
                      const std::string& message_id,
                      const std::string& emoji);
    bool remove_own_reaction(const std::string& channel_id,
                             const std::string& message_id,
                             const std::string& emoji);
    bool remove_user_reaction(const std::string& channel_id,
                              const std::string& message_id,
                              const std::string& emoji,
                              const std::string& user_id);
    bool remove_all_reactions(const std::string& channel_id,
                              const std::string& message_id);
    bool fetch_reactions(const std::string& channel_id,
                         const std::string& message_id,
                         const std::string& emoji,
                         nlohmann::json& out);

    // ─── Attachments / multipart ─────────────────────────────────────────
    // Build a multipart/form-data body for message + files. Returned body
    // has the given boundary. Visible for tests.
    static std::string build_multipart_body(const nlohmann::json& payload_json,
                                            const std::vector<AttachmentSpec>& files,
                                            const std::string& boundary);
    // Validate attachments against Discord limits (default 25MB/file for
    // non-boosted guilds). Returns empty string on success, or human-
    // readable error message.
    static std::string validate_attachments(const std::vector<AttachmentSpec>& files,
                                            std::size_t max_bytes_each = 25 * 1024 * 1024);
    bool send_attachments(const std::string& channel_id,
                          const std::vector<AttachmentSpec>& files,
                          const std::string& content = "");
    // Send a "native" voice message (flag 8192).
    bool send_voice_message(const std::string& channel_id,
                            const std::vector<uint8_t>& ogg_opus,
                            double duration_secs);

    // ─── Stickers ────────────────────────────────────────────────────────
    bool fetch_guild_stickers(const std::string& guild_id,
                              nlohmann::json& out);
    bool send_sticker(const std::string& channel_id,
                      const std::string& sticker_id,
                      const std::string& content = "");

    // ─── Guild / DM ──────────────────────────────────────────────────────
    // Open a DM channel with a user. Returns the dm channel ID on success.
    std::optional<std::string> open_dm(const std::string& user_id);
    bool send_dm(const std::string& user_id, const std::string& content);
    bool fetch_member(const std::string& guild_id,
                      const std::string& user_id,
                      nlohmann::json& out);

    // ─── Presence ────────────────────────────────────────────────────────
    // "online" | "idle" | "dnd" | "invisible"; sends op 3 via gateway.
    bool set_presence(const std::string& status,
                      const std::string& activity_name = "",
                      int activity_type = 0);

    // ─── Webhook mode ────────────────────────────────────────────────────
    void set_webhook(DiscordWebhook wh) { webhook_ = std::move(wh); }
    const std::optional<DiscordWebhook>& webhook() const { return webhook_; }
    bool send_via_webhook(const std::string& content,
                          const std::string& username = "",
                          const std::string& avatar_url = "");
    // Execute the webhook with an embed.
    bool send_webhook_embed(const DiscordEmbed& embed,
                            const std::string& content = "",
                            const std::string& username = "");

    // ─── Rate limiting ────────────────────────────────────────────────────
    // Update a bucket from response headers (visible for tests).
    void update_bucket_from_headers(
        const std::string& bucket_key,
        const std::unordered_map<std::string, std::string>& headers);
    // Should we wait before issuing the next request for this bucket?
    // Returns seconds to wait (0 means go now).
    double wait_seconds_for(const std::string& bucket_key) const;
    // Record a 429 globally.
    void record_global_rate_limit(double retry_after_seconds);
    bool globally_limited() const;
    std::size_t tracked_bucket_count() const;

    // ─── Voice ────────────────────────────────────────────────────────────
    bool join_voice(const std::string& channel_id);
    void leave_voice();
    bool voice_connected() const { return voice_connected_; }
    void set_voice_callback(VoiceCallback cb);
    bool has_voice_callback() const;
    void register_ssrc_user(uint32_t ssrc, const std::string& user_id);
    std::optional<std::string> ssrc_to_user(uint32_t ssrc) const;
    bool send_voice_pcm(const int16_t* pcm, std::size_t frames);
    bool process_voice_rtp(const uint8_t* rtp, std::size_t len);
    bool decrypt_voice_payload(const uint8_t* ciphertext,
                               std::size_t ct_len,
                               const uint8_t* nonce,
                               std::vector<uint8_t>& out_plain) const;
    OpusCodec& voice_codec() { return voice_codec_; }
    // Voice-channel idle tracking (mirrors _reset_voice_timeout /
    // _voice_timeout_handler in the Python adapter).
    void touch_voice_activity();
    bool voice_idle_expired() const;
    std::string voice_channel_id() const { return voice_channel_id_; }

    // ─── Config / state ──────────────────────────────────────────────────
    Config config() const { return cfg_; }
    bool connected() const { return connected_; }
    // Thread participation tracking (mirrors _participated_threads set).
    void track_thread(const std::string& thread_id);
    bool thread_tracked(const std::string& thread_id) const;
    std::size_t tracked_thread_count() const;

    // ─── Gateway WebSocket (v10) ─────────────────────────────────────────
    using MessageCallback = std::function<void(const std::string& channel_id,
                                               const std::string& user_id,
                                               const std::string& content,
                                               const std::string& message_id)>;

    void configure_gateway(int intents,
                           std::unique_ptr<WebSocketTransport> transport = nullptr);
    bool start_gateway();
    void stop_gateway();
    bool gateway_ready() const {
        return gateway_ && gateway_->ready();
    }
    DiscordGateway* gateway() { return gateway_.get(); }
    void set_message_callback(MessageCallback cb) {
        message_cb_ = std::move(cb);
    }
    bool gateway_run_once();
    bool gateway_resume();

    // Compute the intents bitmask from the per-boolean cfg flags. Mirrors
    // the Python call `discord.Intents(...)` with message_content / members /
    // voice_states toggled.
    int compute_intents() const;

private:
    hermes::llm::HttpTransport* get_transport();

    // Low-level REST helpers that all public methods funnel through. These
    // centralise: auth header, base URL, rate-limit bucket accounting.
    hermes::llm::HttpTransport::Response do_get(const std::string& url);
    hermes::llm::HttpTransport::Response do_post_json(
        const std::string& url, const nlohmann::json& body);
    hermes::llm::HttpTransport::Response do_post_empty(const std::string& url);
    hermes::llm::HttpTransport::Response do_delete(const std::string& url);
    hermes::llm::HttpTransport::Response do_put_json(
        const std::string& url, const nlohmann::json& body);
    hermes::llm::HttpTransport::Response do_patch_json(
        const std::string& url, const nlohmann::json& body);
    hermes::llm::HttpTransport::Response do_post_multipart(
        const std::string& url,
        const std::string& boundary,
        const std::string& body);

    // Derive a bucket key from the URL path (coarse — matches major-route
    // granularity without needing the X-RateLimit-Bucket round-trip).
    static std::string bucket_for(const std::string& url);

    Config cfg_;
    hermes::llm::HttpTransport* transport_ = nullptr;
    bool connected_ = false;

    std::unique_ptr<DiscordGateway> gateway_;
    MessageCallback message_cb_;
    InteractionCallback interaction_cb_;

    std::vector<SlashCommand> slash_commands_;
    std::optional<DiscordWebhook> webhook_;

    // Rate-limit state.
    mutable std::mutex rl_mu_;
    std::unordered_map<std::string, RateLimitBucket> buckets_;
    std::chrono::steady_clock::time_point global_reset_ =
        std::chrono::steady_clock::now();
    bool global_limited_ = false;

    // Thread tracking.
    mutable std::mutex thread_mu_;
    std::unordered_set<std::string> participated_threads_;

    // Voice state.
    OpusCodec voice_codec_;
    bool voice_connected_ = false;
    std::string voice_channel_id_;
    std::chrono::steady_clock::time_point voice_last_activity_ =
        std::chrono::steady_clock::now();
    mutable std::mutex ssrc_mu_;
    std::map<uint32_t, std::string> ssrc_map_;
    mutable std::mutex voice_cb_mu_;
    VoiceCallback voice_cb_;
};

}  // namespace hermes::gateway::platforms
