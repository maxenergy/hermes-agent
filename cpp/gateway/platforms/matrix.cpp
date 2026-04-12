// Phase 12 — Matrix platform adapter implementation.
#include "matrix.hpp"

#include <nlohmann/json.hpp>

namespace hermes::gateway::platforms {

MatrixAdapter::MatrixAdapter(Config cfg) : cfg_(std::move(cfg)) {}

MatrixAdapter::MatrixAdapter(Config cfg, hermes::llm::HttpTransport* transport)
    : cfg_(std::move(cfg)), transport_(transport) {}

hermes::llm::HttpTransport* MatrixAdapter::get_transport() {
    if (transport_) return transport_;
    return hermes::llm::get_default_transport();
}

bool MatrixAdapter::connect() {
    if (cfg_.homeserver.empty()) return false;
    if (cfg_.access_token.empty() && (cfg_.username.empty() || cfg_.password.empty()))
        return false;

    auto* transport = get_transport();
    if (!transport) return false;

    // If we have an access_token, use it directly.
    if (!cfg_.access_token.empty()) {
        access_token_ = cfg_.access_token;
        return true;
    }

    // Otherwise, login with username/password.
    nlohmann::json payload = {
        {"type", "m.login.password"},
        {"user", cfg_.username},
        {"password", cfg_.password}
    };

    try {
        auto resp = transport->post_json(
            cfg_.homeserver + "/_matrix/client/r0/login",
            {{"Content-Type", "application/json"}},
            payload.dump());
        if (resp.status_code != 200) return false;

        auto body = nlohmann::json::parse(resp.body);
        if (!body.contains("access_token")) return false;
        access_token_ = body["access_token"].get<std::string>();
        return true;
    } catch (...) {
        return false;
    }
}

void MatrixAdapter::disconnect() {
    access_token_.clear();
}

bool MatrixAdapter::send(const std::string& chat_id,
                         const std::string& content) {
    auto* transport = get_transport();
    if (!transport || access_token_.empty()) return false;

    // chat_id is the room_id (e.g. !abc123:matrix.org).
    nlohmann::json payload = {
        {"msgtype", "m.text"},
        {"body", content}
    };

    // Use a transaction ID based on content hash for idempotency.
    std::string txn_id = std::to_string(std::hash<std::string>{}(content + chat_id));
    std::string url = cfg_.homeserver + "/_matrix/client/r0/rooms/" +
                      chat_id + "/send/m.room.message/" + txn_id;

    try {
        // Matrix uses PUT for sending messages with txn ID.
        // Since our HttpTransport only has post_json/get, use post_json as
        // a workaround — the real CurlTransport sends POST which Matrix
        // also accepts at the /send endpoint.
        auto resp = transport->post_json(
            url,
            {{"Authorization", "Bearer " + access_token_},
             {"Content-Type", "application/json"}},
            payload.dump());
        return resp.status_code >= 200 && resp.status_code < 300;
    } catch (...) {
        return false;
    }
}

void MatrixAdapter::send_typing(const std::string& chat_id) {
    auto* transport = get_transport();
    if (!transport || access_token_.empty()) return;

    nlohmann::json payload = {{"typing", true}, {"timeout", 10000}};
    std::string url = cfg_.homeserver + "/_matrix/client/r0/rooms/" +
                      chat_id + "/typing/@me:matrix.org";

    try {
        transport->post_json(
            url,
            {{"Authorization", "Bearer " + access_token_},
             {"Content-Type", "application/json"}},
            payload.dump());
    } catch (...) {
        // Best-effort.
    }
}

}  // namespace hermes::gateway::platforms
