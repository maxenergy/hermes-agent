// Phase 12 — Slack platform adapter.
#pragma once

#include <functional>
#include <memory>
#include <optional>
#include <string>

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
    };

    explicit SlackAdapter(Config cfg);
    SlackAdapter(Config cfg, hermes::llm::HttpTransport* transport);

    Platform platform() const override { return Platform::Slack; }
    bool connect() override;
    void disconnect() override;
    bool send(const std::string& chat_id, const std::string& content) override;
    void send_typing(const std::string& chat_id) override;

    // Compute Slack request signature: HMAC-SHA256 of "v0:{timestamp}:{body}".
    static std::string compute_slack_signature(const std::string& signing_secret,
                                               const std::string& timestamp,
                                               const std::string& body);

    // Extract thread_ts from an incoming Slack event payload — present
    // only on replies inside a thread.
    static std::optional<std::string> parse_thread_ts(
        const nlohmann::json& event);

    // Decide whether the bot should respond to a Slack message event.
    // Returns true when the event is in a DM, in a thread the bot has
    // already replied in (thread_ts present and matches bot_user_id when
    // supplied), or contains an explicit @-mention of `bot_user_id`.
    // bot_user_id may be empty — in that case any thread reply is treated
    // as in-scope and only DMs are considered direct.
    static bool should_handle_event(const nlohmann::json& event,
                                    const std::string& bot_user_id);

    // Send a message as a reply inside a thread.
    bool send_thread_reply(const std::string& chat_id,
                           const std::string& thread_ts,
                           const std::string& content);

    // Upload a file to a channel via the Slack files.upload endpoint.
    bool upload_file(const std::string& chat_id,
                     const std::string& filename,
                     const std::string& content,
                     const std::string& initial_comment = "");

    Config config() const { return cfg_; }
    bool connected() const { return connected_; }

    // ----- Socket Mode / RTM realtime --------------------------------
    // Callback signature: (channel_id, user_id, text, ts).
    using MessageCallback = std::function<void(const std::string& channel_id,
                                               const std::string& user_id,
                                               const std::string& text,
                                               const std::string& ts)>;

    // Configure realtime driver. If ws_url is empty, the adapter will
    // call Slack REST to obtain it before start_realtime().
    void configure_realtime(
        const std::string& ws_url,
        std::unique_ptr<WebSocketTransport> transport = nullptr);

    // Open the Slack WebSocket and begin receiving events. Calls
    // apps.connections.open (Socket Mode) or rtm.connect (RTM) if no
    // URL was provided at configure-time.
    bool start_realtime();
    void stop_realtime();
    bool realtime_open() const {
        return socket_mode_ && socket_mode_->is_open();
    }
    SlackSocketMode* socket_mode() { return socket_mode_.get(); }

    void set_message_callback(MessageCallback cb) {
        message_cb_ = std::move(cb);
    }

    // Drive one poll of the WebSocket.
    bool realtime_run_once();

    // Fetch WSS URL via apps.connections.open (xapp token) or
    // rtm.connect (xoxb token). Exposed for tests.
    std::optional<std::string> fetch_ws_url();

private:
    hermes::llm::HttpTransport* get_transport();
    Config cfg_;
    hermes::llm::HttpTransport* transport_ = nullptr;
    bool connected_ = false;

    std::unique_ptr<SlackSocketMode> socket_mode_;
    MessageCallback message_cb_;
};

}  // namespace hermes::gateway::platforms
