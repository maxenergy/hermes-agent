// Discord Gateway v10 driver implementation.
#include "discord_gateway.hpp"

namespace hermes::gateway::platforms {

DiscordGateway::DiscordGateway(Config cfg) : cfg_(std::move(cfg)) {}
DiscordGateway::~DiscordGateway() { disconnect(); }

void DiscordGateway::set_transport(std::unique_ptr<WebSocketTransport> t) {
    transport_ = std::move(t);
}

nlohmann::json DiscordGateway::build_identify_payload(const Config& cfg) {
    nlohmann::json props = {
        {"os", "linux"},
        {"browser", "hermes-agent"},
        {"device", "hermes-agent"}
    };
    nlohmann::json presence = {
        {"status", cfg.presence_status},
        {"since", nullptr},
        {"activities", nlohmann::json::array()},
        {"afk", false}
    };
    nlohmann::json d = {
        {"token", cfg.token},
        {"intents", cfg.intents},
        {"properties", props},
        {"compress", false},
        {"large_threshold", 50},
        {"presence", presence}
    };
    if (cfg.shard_count > 1) {
        d["shard"] = nlohmann::json::array({cfg.shard_id, cfg.shard_count});
    }
    return {
        {"op", OP_IDENTIFY},
        {"d", d}
    };
}

nlohmann::json DiscordGateway::build_resume_payload(
    const std::string& token, const std::string& session_id,
    std::int64_t seq) {
    return {
        {"op", OP_RESUME},
        {"d", {
            {"token", token},
            {"session_id", session_id},
            {"seq", seq}
        }}
    };
}

bool DiscordGateway::connect() {
    if (!transport_) {
        transport_ = make_beast_websocket_transport();
    }
    if (!transport_) return false;

    // Discord gateway: wss://gateway.discord.gg/?v=10&encoding=json
    if (!transport_->connect("gateway.discord.gg", "443",
                             "/?v=10&encoding=json")) {
        return false;
    }

    transport_->set_message_callback(
        [this](const std::string& msg) { handle_frame(msg); });

    // Wait for HELLO (op 10) — one read.
    if (!transport_->poll_one()) return false;
    if (heartbeat_interval_ms_ <= 0) return false;

    // Send IDENTIFY.
    if (!send_identify()) return false;
    last_heartbeat_ = std::chrono::steady_clock::now();
    return true;
}

bool DiscordGateway::resume() {
    if (!transport_ || session_id_.empty()) return false;
    auto frame = build_resume_payload(cfg_.token, session_id_, last_seq_);
    return transport_->send_text(frame.dump());
}

void DiscordGateway::disconnect() {
    if (transport_) transport_->close();
    ready_ = false;
    heartbeat_interval_ms_ = 0;
}

bool DiscordGateway::send_identify() {
    if (!transport_) return false;
    auto frame = build_identify_payload(cfg_);
    return transport_->send_text(frame.dump());
}

bool DiscordGateway::send_heartbeat() {
    if (!transport_) return false;
    nlohmann::json frame = {
        {"op", OP_HEARTBEAT},
        {"d", last_seq_ < 0 ? nlohmann::json(nullptr)
                             : nlohmann::json(last_seq_)}
    };
    heartbeat_ack_ = false;  // waiting for ACK
    last_heartbeat_ = std::chrono::steady_clock::now();
    return transport_->send_text(frame.dump());
}

bool DiscordGateway::run_once() {
    if (!transport_ || !transport_->is_open()) return false;

    // Heartbeat pacing.
    if (heartbeat_interval_ms_ > 0) {
        auto now = std::chrono::steady_clock::now();
        auto elapsed =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                now - last_heartbeat_).count();
        if (elapsed >= heartbeat_interval_ms_) {
            send_heartbeat();
        }
    }

    return transport_->poll_one();
}

void DiscordGateway::handle_frame(const std::string& payload) {
    nlohmann::json frame;
    try {
        frame = nlohmann::json::parse(payload);
    } catch (...) {
        return;
    }
    if (!frame.is_object() || !frame.contains("op")) return;

    int op = frame["op"].get<int>();
    if (frame.contains("s") && !frame["s"].is_null()) {
        last_seq_ = frame["s"].get<std::int64_t>();
    }

    switch (op) {
        case OP_HELLO: {
            if (frame.contains("d") && frame["d"].contains("heartbeat_interval")) {
                heartbeat_interval_ms_ =
                    frame["d"]["heartbeat_interval"].get<int>();
            }
            break;
        }
        case OP_HEARTBEAT: {
            // Server requested immediate heartbeat.
            send_heartbeat();
            break;
        }
        case OP_HEARTBEAT_ACK: {
            heartbeat_ack_ = true;
            break;
        }
        case OP_RECONNECT: {
            // Server wants us to reconnect + resume.
            if (transport_) transport_->close();
            break;
        }
        case OP_INVALID_SESSION: {
            // d = true → resumable; d = false → fresh identify required.
            bool resumable = frame.contains("d") && frame["d"].is_boolean() &&
                             frame["d"].get<bool>();
            if (!resumable) {
                session_id_.clear();
                last_seq_ = -1;
            }
            break;
        }
        case OP_DISPATCH: {
            std::string event = frame.value("t", "");
            nlohmann::json data = frame.contains("d") ? frame["d"]
                                                      : nlohmann::json::object();
            if (event == "READY") {
                ready_ = true;
                if (data.contains("session_id")) {
                    session_id_ = data["session_id"].get<std::string>();
                }
                if (data.contains("resume_gateway_url")) {
                    resume_gateway_url_ =
                        data["resume_gateway_url"].get<std::string>();
                }
            } else if (event == "RESUMED") {
                ready_ = true;
            }
            if (dispatch_cb_) dispatch_cb_(event, data);
            break;
        }
        default:
            break;
    }
}

}  // namespace hermes::gateway::platforms
