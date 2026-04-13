// Slack Socket Mode / RTM driver implementation.
#include "slack_socket_mode.hpp"

#include <algorithm>

namespace hermes::gateway::platforms {

static SlackSocketMode::Mode detect_mode(const std::string& app_tok,
                                         const std::string& bot_tok) {
    if (!app_tok.empty() && app_tok.rfind("xapp-", 0) == 0) {
        return SlackSocketMode::Mode::SocketMode;
    }
    // Either an xoxb- bot token or fallback.
    (void)bot_tok;
    return SlackSocketMode::Mode::RTM;
}

SlackSocketMode::SlackSocketMode(Config cfg)
    : cfg_(std::move(cfg)),
      mode_(detect_mode(cfg_.app_token, cfg_.bot_token)) {}

SlackSocketMode::~SlackSocketMode() { disconnect(); }

void SlackSocketMode::set_transport(std::unique_ptr<WebSocketTransport> t) {
    transport_ = std::move(t);
}

void SlackSocketMode::set_websocket_url(const std::string& url) {
    cfg_.ws_url = url;
}

bool SlackSocketMode::parse_wss_url(const std::string& url,
                                     std::string& host,
                                     std::string& port,
                                     std::string& path) {
    const std::string scheme = "wss://";
    if (url.compare(0, scheme.size(), scheme) != 0) return false;
    std::string rest = url.substr(scheme.size());

    auto slash = rest.find('/');
    std::string authority = (slash == std::string::npos)
                                ? rest
                                : rest.substr(0, slash);
    path = (slash == std::string::npos) ? "/" : rest.substr(slash);

    auto colon = authority.find(':');
    if (colon == std::string::npos) {
        host = authority;
        port = "443";
    } else {
        host = authority.substr(0, colon);
        port = authority.substr(colon + 1);
    }
    return !host.empty();
}

bool SlackSocketMode::connect() {
    if (cfg_.ws_url.empty()) return false;
    if (!transport_) {
        transport_ = make_beast_websocket_transport();
    }
    if (!transport_) return false;

    std::string host, port, path;
    if (!parse_wss_url(cfg_.ws_url, host, port, path)) return false;

    if (!transport_->connect(host, port, path)) return false;
    transport_->set_message_callback(
        [this](const std::string& msg) { handle_frame(msg); });
    return true;
}

void SlackSocketMode::disconnect() {
    if (transport_) transport_->close();
}

bool SlackSocketMode::send_json(const nlohmann::json& frame) {
    if (!transport_) return false;
    return transport_->send_text(frame.dump());
}

bool SlackSocketMode::ack_envelope(const std::string& envelope_id) {
    if (envelope_id.empty()) return false;
    nlohmann::json ack = {{"envelope_id", envelope_id}};
    return send_json(ack);
}

bool SlackSocketMode::run_once() {
    if (!transport_ || !transport_->is_open()) return false;
    return transport_->poll_one();
}

void SlackSocketMode::handle_frame(const std::string& payload) {
    nlohmann::json frame;
    try {
        frame = nlohmann::json::parse(payload);
    } catch (...) {
        return;
    }
    if (!frame.is_object()) return;

    if (mode_ == Mode::SocketMode) {
        // Envelope shape: {type, envelope_id, payload, accepts_response_payload}
        std::string type = frame.value("type", "");
        if (type == "hello" || type == "disconnect") {
            if (event_cb_) event_cb_(type, frame);
            return;
        }
        std::string envelope_id = frame.value("envelope_id", "");
        // Auto-ack envelopes that require it. Slack expects ACK to arrive
        // within 3s of delivery.
        if (!envelope_id.empty()) {
            ack_envelope(envelope_id);
        }
        if (event_cb_) {
            nlohmann::json data = frame.contains("payload")
                                      ? frame["payload"]
                                      : nlohmann::json::object();
            event_cb_(type, data);
        }
        return;
    }

    // RTM mode: raw event shape, ping/pong at transport level.
    std::string type = frame.value("type", "");
    if (type == "ping") {
        nlohmann::json pong = {{"type", "pong"},
                               {"id", frame.value("id", 0)}};
        send_json(pong);
        return;
    }
    if (type == "pong") {
        if (event_cb_) event_cb_(type, frame);
        return;
    }
    if (event_cb_) event_cb_(type, frame);
}

}  // namespace hermes::gateway::platforms
