// Phase 9: Minimal single-shot WebSocket client for CDP communication.
//
// Supports ws:// (not wss://), text frames < 64KB, one request-response cycle.
// This is intentionally minimal — a real WebSocket library (Boost.Beast)
// comes in Phase 10+.
#pragma once

#include <chrono>
#include <memory>
#include <string>

namespace hermes::tools {

struct WsResponse {
    bool success = false;
    std::string data;
    std::string error;
};

/// Open a WebSocket connection to `url`, send `message`, read one response, close.
/// Only supports ws:// URLs.  Text frames only, max ~64KB payload.
WsResponse ws_send_recv(const std::string& url, const std::string& message,
                        std::chrono::seconds timeout = std::chrono::seconds(30));

/// Persistent single-client WebSocket connection.
///
/// Used when a protocol requires more than a single request/response cycle on
/// the same socket (e.g. CDP's ``Target.attachToTarget`` + dispatch over the
/// returned sessionId, which has to share the same browser-level WS).
///
/// Only ws:// is supported.  Text frames only.  Not thread-safe.
class WsConnection {
public:
    WsConnection();
    ~WsConnection();

    WsConnection(const WsConnection&) = delete;
    WsConnection& operator=(const WsConnection&) = delete;

    /// Connect + HTTP Upgrade handshake.  Returns false + populates ``error``
    /// when the handshake fails.  ``timeout`` applies to each socket call.
    bool connect(const std::string& url,
                 std::chrono::seconds timeout = std::chrono::seconds(30));

    /// Send a single masked text frame.  Returns false on write failure.
    bool send_text(const std::string& payload);

    /// Read one inbound frame (text).  Returns false on error/timeout.  The
    /// payload is written to ``out``.  Control frames (ping/close) are
    /// handled transparently.
    bool recv_text(std::string& out,
                   std::chrono::seconds timeout = std::chrono::seconds(30));

    /// Last error message (empty when no error).
    const std::string& error() const { return error_; }

    /// Send a WebSocket close frame + shut down the socket.
    void close();

private:
    int fd_;
    std::string error_;
};

}  // namespace hermes::tools
