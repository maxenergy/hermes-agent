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
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <nlohmann/json.hpp>

#include "hermes/acp/permissions.hpp"

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

    // Register a prompt handler — invoked for `prompt` / `session/prompt`
    // RPCs.  Receives the params object and returns the response payload
    // (or an object with an `error` field).
    using PromptHandler =
        std::function<nlohmann::json(const nlohmann::json& params)>;
    void set_prompt_handler(PromptHandler handler);

    // Signal a pending prompt should be cancelled — the adapter flips a
    // cancel flag that long-running prompt handlers can poll via
    // is_cancelled(prompt_id).  No-op if the id is unknown.
    void cancel_prompt(const std::string& prompt_id);
    bool is_cancelled(const std::string& prompt_id) const;

    // Permission matrix (per-session) ------------------------------------
    //
    // The Gateway / tool-dispatch layer consults check_permission() before
    // actually touching the filesystem / shell / network / memory store.
    // RPC methods `session/get_permissions` and `session/set_permissions`
    // let the client inspect and replace the matrix at runtime.
    //
    // If no matrix was explicitly installed for the session, the default
    // PermissionMatrix (conservative defaults, no rules) is consulted.
    PermissionDecision check_permission(const std::string& session_id,
                                        PermissionScope scope,
                                        const nlohmann::json& context) const;

    // Direct accessors — primarily for tests and programmatic setup.
    // Returns a snapshot (copy) of the session's matrix, or the default
    // matrix if the session has none installed.
    PermissionMatrix get_permissions(const std::string& session_id) const;
    void set_permissions(const std::string& session_id,
                         PermissionMatrix matrix);
    bool has_permission_matrix(const std::string& session_id) const;

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

    mutable std::mutex prompt_mu_;
    PromptHandler prompt_handler_;
    std::unordered_set<std::string> cancelled_prompts_;

    // Per-session permission matrices.  Absence in the map means "use
    // the default matrix".  The outer map is guarded by perms_mu_; each
    // PermissionMatrix also has its own mutex for fine-grained access.
    mutable std::mutex perms_mu_;
    std::unordered_map<std::string, PermissionMatrix> permissions_;
};

}  // namespace hermes::acp
