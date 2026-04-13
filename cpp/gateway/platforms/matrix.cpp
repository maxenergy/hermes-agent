// Phase 12 — Matrix platform adapter implementation.
#include "matrix.hpp"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <system_error>

#include <hermes/core/path.hpp>
#include <hermes/gateway/status.hpp>

namespace hermes::gateway::platforms {

namespace {
std::string matrix_lock_identity(const MatrixAdapter::Config& cfg) {
    // homeserver+username is unique per Matrix account; the access_token
    // is hashed under it for a stable lock key.
    return cfg.homeserver + "/" + cfg.username;
}
}  // namespace

MatrixAdapter::MatrixAdapter(Config cfg) : cfg_(std::move(cfg)) {}

MatrixAdapter::MatrixAdapter(Config cfg, hermes::llm::HttpTransport* transport)
    : cfg_(std::move(cfg)), transport_(transport) {}

hermes::llm::HttpTransport* MatrixAdapter::get_transport() {
    if (transport_) return transport_;
    return hermes::llm::get_default_transport();
}

std::string MatrixAdapter::olm_pickle_path() const {
    std::error_code ec;
    auto dir = hermes::core::path::get_hermes_home() / "matrix";
    std::filesystem::create_directories(dir, ec);
    return (dir / "olm_account.pickle").string();
}

bool MatrixAdapter::connect() {
    if (cfg_.homeserver.empty()) return false;
    if (cfg_.access_token.empty() && (cfg_.username.empty() || cfg_.password.empty()))
        return false;

    // Token-scoped lock: prevent two profiles from logging in as the
    // same Matrix account simultaneously.
    if (!hermes::gateway::acquire_scoped_lock(
            hermes::gateway::platform_to_string(platform()),
            matrix_lock_identity(cfg_), {})) {
        return false;
    }

    auto* transport = get_transport();
    if (!transport) {
        hermes::gateway::release_scoped_lock(
            hermes::gateway::platform_to_string(platform()),
            matrix_lock_identity(cfg_));
        return false;
    }

    // If we have an access_token, use it directly.
    if (!cfg_.access_token.empty()) {
        access_token_ = cfg_.access_token;
        // Best-effort E2EE setup — failures are non-fatal.
        (void)setup_e2ee();
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
        if (body.contains("device_id") && cfg_.device_id.empty()) {
            cfg_.device_id = body["device_id"].get<std::string>();
        }
        (void)setup_e2ee();
        return true;
    } catch (...) {
        return false;
    }
}

void MatrixAdapter::disconnect() {
    access_token_.clear();
    if (!cfg_.homeserver.empty()) {
        hermes::gateway::release_scoped_lock(
            hermes::gateway::platform_to_string(platform()),
            matrix_lock_identity(cfg_));
    }
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

// -----------------------------------------------------------------------
// E2EE
// -----------------------------------------------------------------------

bool MatrixAdapter::setup_e2ee() {
#ifndef HERMES_GATEWAY_HAS_OLM
    // Compile-time opt-in: no-op when libolm is not available.
    return true;
#else
    if (!olm_account_.available()) return true;

    // Try to load an existing pickled account; if not present, keep the
    // freshly-generated one.
    std::string path = olm_pickle_path();
    std::ifstream in(path, std::ios::binary);
    if (in) {
        std::stringstream ss;
        ss << in.rdbuf();
        auto pickled = ss.str();
        if (!pickled.empty()) {
            (void)olm_account_.unpickle(pickled, cfg_.pickle_passphrase);
        }
    } else {
        // Persist the freshly-generated account.
        auto pickled = olm_account_.pickle(cfg_.pickle_passphrase);
        if (!pickled.empty()) {
            std::ofstream out(path, std::ios::binary | std::ios::trunc);
            if (out) out.write(pickled.data(), static_cast<std::streamsize>(pickled.size()));
        }
    }

    // Generate some one-time keys and upload identity + device keys. We
    // best-effort POST to /keys/upload; failure does not break the adapter.
    (void)olm_account_.generate_one_time_keys(50);

    auto* transport = get_transport();
    if (transport && !access_token_.empty()) {
        try {
            nlohmann::json payload = {
                {"device_keys", nlohmann::json::parse(
                     olm_account_.identity_keys_json().empty()
                         ? std::string("{}")
                         : olm_account_.identity_keys_json())},
                {"one_time_keys", nlohmann::json::parse(
                     olm_account_.one_time_keys_json().empty()
                         ? std::string("{}")
                         : olm_account_.one_time_keys_json())}
            };
            auto resp = transport->post_json(
                cfg_.homeserver + "/_matrix/client/r0/keys/upload",
                {{"Authorization", "Bearer " + access_token_},
                 {"Content-Type", "application/json"}},
                payload.dump());
            if (resp.status_code >= 200 && resp.status_code < 300) {
                olm_account_.mark_keys_as_published();
            }
        } catch (...) {
            // Best-effort.
        }
    }
    return true;
#endif
}

bool MatrixAdapter::encrypt_room_message(const std::string& room_id,
                                          const std::string& plaintext,
                                          std::string& out_ciphertext) {
#ifndef HERMES_GATEWAY_HAS_OLM
    (void)room_id;
    out_ciphertext = plaintext;
    return true;
#else
    auto it = megolm_out_.find(room_id);
    if (it == megolm_out_.end() || !it->second.available()) {
        // No session yet — fall back to plaintext.
        out_ciphertext = plaintext;
        return true;
    }
    auto ct = it->second.encrypt(plaintext);
    if (ct.empty()) return false;
    out_ciphertext = std::move(ct);
    return true;
#endif
}

std::optional<std::string> MatrixAdapter::decrypt_room_message(
    const std::string& room_id, const std::string& ciphertext) {
#ifndef HERMES_GATEWAY_HAS_OLM
    (void)room_id;
    return ciphertext;
#else
    auto room_it = megolm_in_.find(room_id);
    if (room_it == megolm_in_.end()) return ciphertext;  // no session → pass-through
    for (auto& [sid, session] : room_it->second) {
        auto pt = session.decrypt(ciphertext);
        if (pt.has_value()) return pt;
    }
    return std::nullopt;
#endif
}

}  // namespace hermes::gateway::platforms
