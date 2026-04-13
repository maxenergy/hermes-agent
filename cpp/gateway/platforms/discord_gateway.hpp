// Discord Gateway v10 protocol driver. Wraps a WebSocketTransport and
// implements the opcode state machine: HELLO (10) -> IDENTIFY (2) ->
// READY -> heartbeat loop (1/11) with RESUME (6) on reconnect.
//
// Events are not decoded end-to-end here; MESSAGE_CREATE and READY are
// surfaced to the adapter via callbacks, which routes them into the
// existing inbound message path.
#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>

#include <nlohmann/json.hpp>

#include "websocket_transport.hpp"

namespace hermes::gateway::platforms {

class DiscordGateway {
public:
    // Discord gateway opcodes (v10).
    enum Opcode : int {
        OP_DISPATCH = 0,
        OP_HEARTBEAT = 1,
        OP_IDENTIFY = 2,
        OP_PRESENCE_UPDATE = 3,
        OP_VOICE_STATE_UPDATE = 4,
        OP_RESUME = 6,
        OP_RECONNECT = 7,
        OP_REQUEST_GUILD_MEMBERS = 8,
        OP_INVALID_SESSION = 9,
        OP_HELLO = 10,
        OP_HEARTBEAT_ACK = 11,
    };

    struct Config {
        std::string token;       // Bot token
        int intents = 0;         // Bitfield; 0 = minimal (guilds + messages)
        int shard_id = 0;
        int shard_count = 1;
        std::string presence_status = "online";
    };

    // DISPATCH callback: (event_name, data_json).
    using DispatchCallback = std::function<void(const std::string& event,
                                                const nlohmann::json& data)>;

    explicit DiscordGateway(Config cfg);
    ~DiscordGateway();

    // Inject a WebSocket transport. Ownership moves in. If not called,
    // a Beast-backed transport is created on connect().
    void set_transport(std::unique_ptr<WebSocketTransport> t);

    WebSocketTransport* transport() { return transport_.get(); }

    // Route dispatched events to the host adapter.
    void set_dispatch_callback(DispatchCallback cb) {
        dispatch_cb_ = std::move(cb);
    }

    // Perform gateway handshake: connect WSS, wait for HELLO, send
    // IDENTIFY. Returns true if all three steps succeed within one
    // poll cycle.
    bool connect();

    // Send a RESUME opcode using last-known session_id + seq. Used when
    // reconnecting after a transient disconnect.
    bool resume();

    // Close the socket and release the session.
    void disconnect();

    // Pump one message from the socket. Invokes dispatch callbacks as
    // needed and sends heartbeat if the interval has elapsed. Returns
    // false when the socket has closed.
    bool run_once();

    // Send an IDENTIFY (opcode 2) frame.
    bool send_identify();

    // Send a HEARTBEAT (opcode 1) frame with the last seen sequence.
    bool send_heartbeat();

    // Inspect current session state.
    const std::string& session_id() const { return session_id_; }
    std::int64_t last_sequence() const { return last_seq_; }
    int heartbeat_interval_ms() const { return heartbeat_interval_ms_; }
    bool ready() const { return ready_; }
    bool heartbeat_ack_received() const { return heartbeat_ack_; }

    // Manually feed the driver a frame (tests).
    void handle_frame(const std::string& payload);

    // Build the identify payload — exposed for tests.
    static nlohmann::json build_identify_payload(const Config& cfg);
    // Build the resume payload.
    static nlohmann::json build_resume_payload(const std::string& token,
                                                const std::string& session_id,
                                                std::int64_t seq);

private:
    Config cfg_;
    std::unique_ptr<WebSocketTransport> transport_;
    DispatchCallback dispatch_cb_;

    std::string session_id_;
    std::string resume_gateway_url_;
    std::int64_t last_seq_ = -1;
    int heartbeat_interval_ms_ = 0;
    bool ready_ = false;
    bool heartbeat_ack_ = true;  // start true so first heartbeat can fire
    std::chrono::steady_clock::time_point last_heartbeat_{};
};

}  // namespace hermes::gateway::platforms
