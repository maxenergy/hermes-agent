// AcpAdapter — editor integration adapter for VS Code / Zed / JetBrains.
//
// Implements a minimal subset of the Agent Client Protocol (ACP):
//   - initialize     -> returns AgentCapabilities + auth_methods
//   - authenticate   -> validates credentials per method_id, stores per-session token
//   - new_session    -> allocates a session id (requires authenticated client)
//   - capabilities   -> returns capability manifest
//
// Auth sources (first wins):
//   1. ANTHROPIC_API_KEY / OPENAI_API_KEY env vars (seeded into env_token_).
//   2. `authenticate` RPC with method "api-key" + params.api_key.
//   3. `authenticate` RPC with method "oauth" + params.access_token.
//
// Once `authenticate` succeeds for a method, subsequent calls that pass a
// matching `session_id` are considered authenticated. Calls without a valid
// session token are rejected with an "unauthenticated" error unless the env
// token was present at construction (in which case all sessions inherit it).
#pragma once

#include <atomic>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include <nlohmann/json.hpp>

namespace hermes::acp {

struct AcpConfig {
    std::string listen_address = "127.0.0.1";
    int listen_port = 8765;
    // If set, bypasses env probing (pass std::nullopt to probe; empty string
    // explicitly disables env-based auth).
    std::optional<std::string> forced_env_token;
};

// Result of an authenticate attempt, exposed for tests.
struct AuthResult {
    bool ok = false;
    std::string session_id;       // token issued to the client on success
    std::string method_id;        // echoed method
    std::string error;            // populated iff !ok
};

class AcpAdapter {
public:
    explicit AcpAdapter(AcpConfig config);

    // Start HTTP server for ACP protocol.
    void start();
    void stop();
    bool running() const;

    // ACP capability registration.
    nlohmann::json capabilities() const;

    // Advertised auth methods (subset of the ACP spec: api-key, oauth).
    nlohmann::json auth_methods() const;

    // RPC handler — dispatches on request["method"].
    nlohmann::json handle_request(const nlohmann::json& request);

    // Direct auth entry point (also driven by handle_request("authenticate")).
    AuthResult authenticate(const std::string& method_id,
                            const nlohmann::json& params);

    // Session token check.
    bool is_authenticated(const std::string& session_id) const;

    // True if an env-provided credential was detected at construction time.
    bool has_env_credential() const { return !env_token_.empty(); }

private:
    // Issue a new opaque session id. Caller holds sessions_mu_.
    std::string mint_session_id_locked() const;

    AcpConfig config_;
    std::atomic<bool> running_{false};

    // Token discovered from environment at construction — grants implicit auth
    // to any session when non-empty.
    std::string env_token_;

    mutable std::mutex sessions_mu_;
    // session_id -> method_id that minted it.
    std::unordered_map<std::string, std::string> sessions_;
};

}  // namespace hermes::acp
