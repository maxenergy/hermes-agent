// Phase 9: Minimal single-shot WebSocket client for CDP communication.
//
// Supports ws:// (not wss://), text frames < 64KB, one request-response cycle.
// This is intentionally minimal — a real WebSocket library (Boost.Beast)
// comes in Phase 10+.
#pragma once

#include <chrono>
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

}  // namespace hermes::tools
