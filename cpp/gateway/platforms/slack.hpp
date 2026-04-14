// Phase 12 — Slack platform adapter.
//
// Expanded C++17 port of gateway/platforms/slack.py covering the Web API
// surface used by the Python adapter: chat.* messaging with Block Kit,
// conversations.* (DM/MPIM/channel info, open, history), users.* (info +
// lookupByEmail + getPresence), reactions.*, pins.*, stars.*, files.*
// (content + multipart upload_v2 flow), views.* (open/push/update/publish),
// chat.postEphemeral / chat.delete / chat.update, mrkdwn formatting, and
// an HTTP rate-limit-aware wrapper that respects `Retry-After` on 429.
#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include <nlohmann/json.hpp>

#include <hermes/gateway/gateway_runner.hpp>
#include <hermes/llm/llm_client.hpp>

#include "slack_socket_mode.hpp"

namespace hermes::gateway::platforms {

class SlackAdapter : public BasePlatformAdapter {
public:
    struct Config {
        std::string bot_token;
        std::string signing_secret;
        std::string app_token;  // for Socket Mode
        std::string bot_user_id;
        std::string team_id;
        std::string enterprise_id;
        // Maximum retries for rate-limit (429) responses before give-up.
        int max_retries_on_429 = 3;
        // When non-zero, used as the retry delay (ms) for 429 instead of the
        // server-provided Retry-After value. Tests set this to 0 to avoid
        // sleeping while still exercising retry logic.
        std::int64_t retry_after_override_ms = 0;
    };

    explicit SlackAdapter(Config cfg);
    SlackAdapter(Config cfg, hermes::llm::HttpTransport* transport);

    Platform platform() const override { return Platform::Slack; }
    bool connect() override;
    void disconnect() override;
    bool send(const std::string& chat_id, const std::string& content) override;
    void send_typing(const std::string& chat_id) override;

    // ----- Request signing / verification --------------------------------

    // Compute Slack request signature: HMAC-SHA256 of "v0:{timestamp}:{body}".
    static std::string compute_slack_signature(const std::string& signing_secret,
                                               const std::string& timestamp,
                                               const std::string& body);

    // Verify an incoming Events API webhook signature. Rejects requests
    // older than `max_skew_seconds` to block replay attacks.
    static bool verify_events_api_signature(const std::string& signing_secret,
                                            const std::string& timestamp,
                                            const std::string& body,
                                            const std::string& signature,
                                            int max_skew_seconds = 300);

    // ----- Event parsing helpers -----------------------------------------

    // Extract thread_ts from an incoming Slack event payload — present
    // only on replies inside a thread.
    static std::optional<std::string> parse_thread_ts(
        const nlohmann::json& event);

    // Decide whether the bot should respond to a Slack message event.
    static bool should_handle_event(const nlohmann::json& event,
                                    const std::string& bot_user_id);

    // Extract (team_id, enterprise_id) from an Enterprise Grid events
    // wrapper. Either may be empty.
    struct GridIdentity {
        std::string team_id;
        std::string enterprise_id;
        bool is_enterprise_install = false;
    };
    static GridIdentity parse_grid_identity(const nlohmann::json& envelope);

    // Detect message subtype categories the adapter should ignore
    // (message_changed / message_deleted / bot_message / channel_join etc.)
    static bool is_ignorable_subtype(const nlohmann::json& event);

    // ----- Block Kit builders --------------------------------------------

    static nlohmann::json section_block(const std::string& markdown_text,
                                        const std::string& block_id = "");
    static nlohmann::json divider_block();
    static nlohmann::json header_block(const std::string& plain_text);
    static nlohmann::json context_block(
        const std::vector<std::string>& elements);
    static nlohmann::json image_block(const std::string& image_url,
                                      const std::string& alt_text,
                                      const std::string& title = "");
    static nlohmann::json input_block(const std::string& label,
                                      const nlohmann::json& element,
                                      const std::string& block_id = "",
                                      bool optional = false);
    static nlohmann::json actions_block(
        const std::vector<nlohmann::json>& elements,
        const std::string& block_id = "");

    static nlohmann::json button_element(const std::string& text,
                                         const std::string& action_id,
                                         const std::string& value = "",
                                         const std::string& style = "");
    static nlohmann::json static_select_element(
        const std::string& action_id,
        const std::string& placeholder,
        const std::vector<std::pair<std::string, std::string>>& options);
    static nlohmann::json datepicker_element(
        const std::string& action_id,
        const std::string& placeholder,
        const std::string& initial_date = "");
    static nlohmann::json users_select_element(const std::string& action_id,
                                               const std::string& placeholder);
    static nlohmann::json channels_select_element(
        const std::string& action_id, const std::string& placeholder);
    static nlohmann::json plain_text_input_element(
        const std::string& action_id, bool multiline = false);

    // Top-level modal / view envelope.
    static nlohmann::json modal_view(const std::string& title,
                                     const std::vector<nlohmann::json>& blocks,
                                     const std::string& submit = "",
                                     const std::string& close = "",
                                     const std::string& callback_id = "",
                                     const std::string& private_metadata = "");
    static nlohmann::json home_view(const std::vector<nlohmann::json>& blocks);

    // ----- Message operations --------------------------------------------

    bool send_thread_reply(const std::string& chat_id,
                           const std::string& thread_ts,
                           const std::string& content);

    // chat.postMessage with blocks + optional text fallback.
    bool send_blocks(const std::string& chat_id,
                     const std::vector<nlohmann::json>& blocks,
                     const std::string& fallback_text = "",
                     const std::string& thread_ts = "");

    // chat.postEphemeral — message visible only to `user`.
    bool send_ephemeral(const std::string& chat_id,
                        const std::string& user,
                        const std::string& content);

    // chat.update — edit an already-posted message.
    bool update_message(const std::string& chat_id,
                        const std::string& ts,
                        const std::string& content);

    // chat.delete — remove a message by (channel, ts).
    bool delete_message(const std::string& chat_id, const std::string& ts);

    // files.upload (content=) — kept for backward compat with existing tests.
    bool upload_file(const std::string& chat_id,
                     const std::string& filename,
                     const std::string& content,
                     const std::string& initial_comment = "");

    // files.getUploadURLExternal + files.completeUploadExternal flow
    // (Slack's v2 upload, used when files exceed the inline size limit).
    struct UploadExternalResult {
        bool ok = false;
        std::string file_id;
        std::string upload_url;
    };
    UploadExternalResult get_upload_url_external(const std::string& filename,
                                                 std::int64_t length);
    bool complete_upload_external(const std::string& file_id,
                                  const std::string& title,
                                  const std::string& channel_id,
                                  const std::string& initial_comment = "",
                                  const std::string& thread_ts = "");

    // ----- Reactions / pins / stars --------------------------------------

    bool add_reaction(const std::string& channel, const std::string& ts,
                      const std::string& emoji);
    bool remove_reaction(const std::string& channel, const std::string& ts,
                         const std::string& emoji);
    nlohmann::json get_reactions(const std::string& channel,
                                 const std::string& ts);
    bool pin_message(const std::string& channel, const std::string& ts);
    bool unpin_message(const std::string& channel, const std::string& ts);
    bool add_star(const std::string& channel, const std::string& ts);

    // ----- Conversations --------------------------------------------------

    // conversations.open — returns channel id of the opened DM/MPIM.
    std::optional<std::string> open_dm(const std::string& user_id);
    std::optional<std::string> open_mpim(
        const std::vector<std::string>& user_ids);

    // conversations.info / conversations.members / conversations.history.
    nlohmann::json get_channel_info(const std::string& channel_id);
    nlohmann::json get_channel_members(const std::string& channel_id,
                                       int limit = 100);
    nlohmann::json get_channel_history(const std::string& channel_id,
                                       int limit = 20,
                                       const std::string& cursor = "");
    nlohmann::json list_channels(const std::string& types = "public_channel",
                                 int limit = 100);

    // ----- Users ----------------------------------------------------------

    nlohmann::json get_user_info(const std::string& user_id);
    std::optional<std::string> lookup_user_by_email(const std::string& email);
    std::optional<std::string> get_user_presence(const std::string& user_id);

    // ----- Views (modals + App Home) -------------------------------------

    bool views_open(const std::string& trigger_id, const nlohmann::json& view);
    bool views_push(const std::string& trigger_id, const nlohmann::json& view);
    bool views_update(const std::string& view_id, const nlohmann::json& view);
    bool views_publish(const std::string& user_id, const nlohmann::json& view,
                       const std::string& hash = "");

    // ----- mrkdwn formatting ---------------------------------------------

    // Convert standard markdown to Slack mrkdwn (headers → *bold*,
    // **bold** → *text*, [label](url) → <url|label>, ~~strike~~ → ~x~,
    // fenced code / inline code preserved).
    static std::string format_message(const std::string& content);

    Config config() const { return cfg_; }
    bool connected() const { return connected_; }

    // ----- Socket Mode / RTM realtime --------------------------------
    using MessageCallback = std::function<void(const std::string& channel_id,
                                               const std::string& user_id,
                                               const std::string& text,
                                               const std::string& ts)>;
    using InteractionCallback =
        std::function<void(const nlohmann::json& payload)>;

    void configure_realtime(
        const std::string& ws_url,
        std::unique_ptr<WebSocketTransport> transport = nullptr);

    bool start_realtime();
    void stop_realtime();
    bool realtime_open() const {
        return socket_mode_ && socket_mode_->is_open();
    }
    SlackSocketMode* socket_mode() { return socket_mode_.get(); }

    void set_message_callback(MessageCallback cb) {
        message_cb_ = std::move(cb);
    }
    void set_interaction_callback(InteractionCallback cb) {
        interaction_cb_ = std::move(cb);
    }

    bool realtime_run_once();
    std::optional<std::string> fetch_ws_url();

    // ----- Shortcuts / slash command payload helpers ---------------------

    // Parse the x-www-form-urlencoded body sent by Slack for a slash command
    // into a command→value map. The Slack slash command body fields include
    // token, team_id, channel_id, user_id, command, text, response_url,
    // trigger_id, api_app_id, enterprise_id.
    static std::unordered_map<std::string, std::string> parse_slash_form(
        const std::string& body);

    // Test-only accessors.
    int last_retry_count() const { return last_retry_count_; }

private:
    hermes::llm::HttpTransport* get_transport();

    // Low-level POST with rate-limit (429 Retry-After) retry handling.
    // Returns the final Response; populates `last_retry_count_` with how
    // many 429 responses were observed before success or give-up.
    hermes::llm::HttpTransport::Response post_api(
        const std::string& method,
        const nlohmann::json& payload);

    // GET variant (conversations.history etc.) with query params.
    hermes::llm::HttpTransport::Response get_api(
        const std::string& method,
        const std::unordered_map<std::string, std::string>& query);

    static std::string urlencode(const std::string& s);
    static std::string build_query_string(
        const std::unordered_map<std::string, std::string>& params);

    Config cfg_;
    hermes::llm::HttpTransport* transport_ = nullptr;
    bool connected_ = false;
    int last_retry_count_ = 0;

    std::unique_ptr<SlackSocketMode> socket_mode_;
    MessageCallback message_cb_;
    InteractionCallback interaction_cb_;
};

}  // namespace hermes::gateway::platforms
