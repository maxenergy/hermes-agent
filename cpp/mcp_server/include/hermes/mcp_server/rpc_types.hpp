// JSON-RPC 2.0 request / response helpers + MCP error code constants.
//
// MCP uses a strict subset of JSON-RPC 2.0 layered on top of stdio / HTTP /
// SSE transports. This header gives the transport + dispatcher a single
// place to construct envelopes and reason about the reserved error codes
// (-32700..-32000) without stringly-typed duplication.
#pragma once

#include <nlohmann/json.hpp>

#include <optional>
#include <string>
#include <string_view>

namespace hermes::mcp_server {

// JSON-RPC 2.0 reserved error codes (spec §5.1) + MCP implementation-defined
// server errors in the -32099..-32000 band.
namespace rpc_error {
inline constexpr int kParseError = -32700;
inline constexpr int kInvalidRequest = -32600;
inline constexpr int kMethodNotFound = -32601;
inline constexpr int kInvalidParams = -32602;
inline constexpr int kInternalError = -32603;
// MCP-specific "server error" band is -32099..-32000. We pick a single
// sentinel for wrapped handler exceptions.
inline constexpr int kServerError = -32000;
}  // namespace rpc_error

// Protocol version string advertised by this server.
inline constexpr std::string_view kProtocolVersion = "2024-11-05";

// A decoded JSON-RPC request envelope. For notifications, ``id`` is
// null/absent and no response should be produced.
struct RpcRequest {
    nlohmann::json id;  // may be null (notification), string, or integer
    std::string method;
    nlohmann::json params;  // object or array; defaults to object
    bool is_notification = false;
    // Parse errors populated here (method == "" and ``parse_error`` set)
    // so the dispatcher can short-circuit with a -32700 envelope.
    std::optional<std::string> parse_error;
};

// Decode a JSON-RPC request from a raw string payload. Never throws;
// malformed payloads yield an ``RpcRequest`` with ``parse_error`` set.
RpcRequest parse_request(std::string_view payload);

// Build a JSON-RPC 2.0 success envelope.
nlohmann::json make_result(const nlohmann::json& id,
                           nlohmann::json result);

// Build a JSON-RPC 2.0 error envelope.
nlohmann::json make_error(const nlohmann::json& id, int code,
                          std::string_view message,
                          nlohmann::json data = nullptr);

}  // namespace hermes::mcp_server
