// WebSocket transport abstraction used by realtime gateway adapters
// (Discord gateway, Slack Socket Mode / RTM). Implementations can swap in
// a mock for unit tests or the Boost.Beast-backed implementation at
// runtime.
#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace hermes::gateway::platforms {

// Abstract full-duplex WebSocket transport. Implementations are expected
// to manage the TLS connection + handshake internally and deliver text
// frames via the supplied message callback.
//
// Thread model: connect() blocks until the socket is live or fails.
// send_text() is safe to call from any thread once connected. The
// message callback may be invoked on a background thread.
class WebSocketTransport {
public:
    using MessageCallback = std::function<void(const std::string&)>;
    using CloseCallback = std::function<void(int code, const std::string& reason)>;

    virtual ~WebSocketTransport() = default;

    // Connect to `wss://host:port/path`. Returns true on successful
    // handshake. TLS cert verification MUST be enabled and use the
    // system CA bundle.
    virtual bool connect(const std::string& host,
                         const std::string& port,
                         const std::string& path) = 0;

    // Gracefully close the connection. Subsequent connect() re-opens.
    virtual void close() = 0;

    // Send a text frame. Returns false if the socket is not open.
    virtual bool send_text(const std::string& payload) = 0;

    // Register a message receiver. Called for each inbound text frame.
    virtual void set_message_callback(MessageCallback cb) = 0;

    // Register a close-notification receiver.
    virtual void set_close_callback(CloseCallback cb) = 0;

    // True iff the socket is connected and reads are being pumped.
    virtual bool is_open() const = 0;

    // Drain one pending message (used by the read-loop driver). Returns
    // false if the socket has closed. The inbound message, if any, is
    // delivered via the message callback.
    virtual bool poll_one() = 0;
};

// Factory returning a mock transport useful for tests.
std::unique_ptr<WebSocketTransport> make_mock_websocket_transport();

// Factory for the Boost.Beast TLS-backed transport. Returns nullptr
// when Beast headers weren't available at build time.
std::unique_ptr<WebSocketTransport> make_beast_websocket_transport();

// Test helpers — operate on a mock instance, no-op on real transport.
void mock_inject_inbound(WebSocketTransport* t, std::string frame);
std::vector<std::string> mock_drain_sent(WebSocketTransport* t);
std::string mock_last_host(WebSocketTransport* t);
std::string mock_last_path(WebSocketTransport* t);

}  // namespace hermes::gateway::platforms
