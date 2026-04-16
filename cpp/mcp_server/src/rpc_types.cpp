#include "hermes/mcp_server/rpc_types.hpp"

#include <string>

namespace hermes::mcp_server {

RpcRequest parse_request(std::string_view payload) {
    RpcRequest req;
    nlohmann::json j;
    try {
        j = nlohmann::json::parse(payload);
    } catch (const std::exception& ex) {
        req.parse_error = std::string("parse error: ") + ex.what();
        return req;
    }

    if (!j.is_object()) {
        req.parse_error = "request must be a JSON object";
        return req;
    }

    if (j.contains("id")) {
        req.id = j["id"];
    } else {
        req.id = nlohmann::json();  // null → notification
        req.is_notification = true;
    }

    if (j.contains("method") && j["method"].is_string()) {
        req.method = j["method"].get<std::string>();
    } else {
        req.parse_error = "missing or non-string \"method\"";
        return req;
    }

    if (j.contains("params")) {
        req.params = j["params"];
        if (!req.params.is_object() && !req.params.is_array()) {
            req.parse_error = "\"params\" must be object or array";
            return req;
        }
    } else {
        req.params = nlohmann::json::object();
    }

    // Strict spec interpretation: notifications have no ``id`` field at
    // all, but in practice many MCP clients send ``id: null`` to mean the
    // same thing. Treat null ids as notifications too.
    if (req.id.is_null()) req.is_notification = true;

    return req;
}

nlohmann::json make_result(const nlohmann::json& id, nlohmann::json result) {
    nlohmann::json env;
    env["jsonrpc"] = "2.0";
    env["id"] = id.is_null() ? nlohmann::json() : id;
    env["result"] = std::move(result);
    return env;
}

nlohmann::json make_error(const nlohmann::json& id, int code,
                          std::string_view message, nlohmann::json data) {
    nlohmann::json env;
    env["jsonrpc"] = "2.0";
    env["id"] = id.is_null() ? nlohmann::json() : id;
    nlohmann::json err;
    err["code"] = code;
    err["message"] = std::string(message);
    if (!data.is_null()) err["data"] = std::move(data);
    env["error"] = std::move(err);
    return env;
}

}  // namespace hermes::mcp_server
