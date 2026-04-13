// Slack Socket Mode driver. Uses an app-level token (xapp-) to open a
// WebSocket URL via apps.connections.open, then streams `events_api`,
// `slash_commands`, and `interactive` envelopes. Each envelope MUST be
// ack'd by sending back {"envelope_id": "..."} on the same socket.
//
// For legacy RTM (bot token xoxb- with users:read), the URL is obtained
// via rtm.connect and events arrive as {"type": "message", ...} without
// envelope wrappers. The driver auto-detects which mode to use based on
// the token prefix.
#pragma once

#include <functional>
#include <memory>
#include <string>

#include <nlohmann/json.hpp>

#include "websocket_transport.hpp"

namespace hermes::gateway::platforms {

class SlackSocketMode {
public:
    enum class Mode {
        SocketMode,  // xapp- token — envelope-based
        RTM,         // xoxb- token — legacy RTM, ping/pong
    };

    struct Config {
        std::string app_token;  // xapp-... for Socket Mode
        std::string bot_token;  // xoxb-... for RTM fallback
        // Pre-opened WSS URL; when empty, the driver is expected to
        // have had it injected via set_websocket_url() (the adapter
        // first calls the REST endpoint to fetch it).
        std::string ws_url;
    };

    // event callback: (event_type, full_envelope_or_payload).
    using EventCallback = std::function<void(const std::string& type,
                                             const nlohmann::json& data)>;

    explicit SlackSocketMode(Config cfg);
    ~SlackSocketMode();

    void set_transport(std::unique_ptr<WebSocketTransport> t);
    WebSocketTransport* transport() { return transport_.get(); }

    void set_event_callback(EventCallback cb) {
        event_cb_ = std::move(cb);
    }

    // Open the websocket. Requires cfg_.ws_url to be set (see
    // set_websocket_url) or will fail.
    bool connect();
    void disconnect();

    // Pump one inbound frame. Returns false when the socket has closed.
    bool run_once();

    // Send a raw JSON frame (used for envelope ACKs or PINGs).
    bool send_json(const nlohmann::json& frame);

    // Acknowledge an envelope by id — required under Socket Mode.
    bool ack_envelope(const std::string& envelope_id);

    // Populate the WSS URL obtained from apps.connections.open or
    // rtm.connect.
    void set_websocket_url(const std::string& url);

    Mode mode() const { return mode_; }
    bool is_open() const {
        return transport_ && transport_->is_open();
    }

    // Test hook — feeds a frame into the driver.
    void handle_frame(const std::string& payload);

    // Parse a wss:// URL into host / port / path components. Returns
    // false if the URL is malformed or not wss://.
    static bool parse_wss_url(const std::string& url,
                              std::string& host,
                              std::string& port,
                              std::string& path);

private:
    Config cfg_;
    Mode mode_;
    std::unique_ptr<WebSocketTransport> transport_;
    EventCallback event_cb_;
    int next_ping_id_ = 1;
};

}  // namespace hermes::gateway::platforms
