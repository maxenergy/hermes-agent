// Phase 12: OAuth 2.1 + PKCE authorization-code flow for MCP servers.
//
// Spawns a transient localhost HTTP listener on ``redirect_uri``, opens the
// user's browser, waits for the ``?code=`` callback, and exchanges it at
// the token endpoint.  Tokens persist under
// ``$HERMES_HOME/mcp/<server_name>.token.json`` (mode 0600).
//
// Ports upstream Python commit 70768665: a shared ``MCPOAuthManager``
// aggregates per-server OAuth state so CLI config-time (``hermes mcp add``)
// and runtime (MCP tool handlers) see the same cache.  The manager also
// watches the on-disk token mtime so that an external refresh (cron job /
// another Hermes process / ``hermes mcp login``) is picked up before the
// next API call, and dedupes concurrent refresh attempts so two in-flight
// tools don't double-refresh.
#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace hermes::llm {
class HttpTransport;
}

namespace hermes::auth {

struct McpOAuthConfig {
    std::string server_url;         // MCP server root URL
    std::string authorization_url;  // ...or explicit /authorize endpoint
    std::string token_url;          // ...or explicit /token endpoint
    std::string client_id;
    std::string client_secret;      // optional (confidential clients)
    std::string redirect_uri;       // typically http://127.0.0.1:PORT/callback
    std::vector<std::string> scopes;
};

struct McpOAuthToken {
    std::string access_token;
    std::string refresh_token;
    int64_t expiry_date_ms = 0;
    std::string token_type = "Bearer";
    std::string scope;
};

class McpOAuth {
public:
    explicit McpOAuth(hermes::llm::HttpTransport* transport = nullptr);

    /// Standard OAuth 2.1 authorization code flow with PKCE.  Blocks until
    /// the callback is received (or the listener times out).
    std::optional<McpOAuthToken> interactive_login(const McpOAuthConfig& cfg);

    /// Exchange a refresh_token for a new access_token.  Returns nullopt
    /// on network / provider error.
    std::optional<McpOAuthToken> refresh(const McpOAuthConfig& cfg,
                                         const McpOAuthToken& current);

    /// Persist a token under ``$HERMES_HOME/mcp/<server_name>.token.json``
    /// with mode 0600.  Returns false on IO error.
    static bool save_token(const std::string& server_name,
                           const McpOAuthToken& tok);

    /// Load a previously-saved token.  Returns nullopt when the file is
    /// missing or malformed.
    static std::optional<McpOAuthToken> load_token(const std::string& server_name);

    // -- Lower-level helpers (exposed for tests) -----------------------

    struct PkceChallenge {
        std::string verifier;  // random high-entropy
        std::string challenge; // base64url(sha256(verifier))
        std::string method = "S256";
    };

    /// Generate a fresh PKCE verifier + S256 challenge.
    static PkceChallenge make_pkce();

    /// Build the full ``/authorize`` URL (client_id, redirect_uri, scope,
    /// code_challenge, code_challenge_method=S256, response_type=code).
    static std::string build_authorize_url(const McpOAuthConfig& cfg,
                                           const PkceChallenge& pkce,
                                           const std::string& state);

    /// Spawn a one-shot listener on ``port`` and return the ``code`` query
    /// param once the browser redirects.  Returns empty string on timeout.
    static std::string listen_for_code(int port, int timeout_sec = 300,
                                       std::string* state_out = nullptr);

    /// Extract port from a redirect_uri like ``http://127.0.0.1:54321/callback``.
    static int extract_port(const std::string& redirect_uri);

private:
    hermes::llm::HttpTransport* transport_;

    std::optional<McpOAuthToken> exchange_code(
        const McpOAuthConfig& cfg,
        const std::string& code,
        const PkceChallenge& pkce);
};

// ----------------------------------------------------------------------------
// MCPOAuthManager — per-server OAuth state aggregator.
//
// Responsibilities (ported from upstream Python commit 70768665):
//  * Per-server provider cache — CLI and runtime share one instance.
//  * mtime watch — when the on-disk ``<server>.token.json`` is touched by
//    an external process, the cached token is reloaded on the next
//    ``get_token`` call and any registered reconnect callback is fired.
//  * Deduped recovery — concurrent refresh attempts funnel through a single
//    condition-variable-guarded slot so we never double-refresh the same
//    refresh_token.
//  * Reconnect signalling — transports register a callback (typically a
//    ``condition_variable::notify_all`` / event set) that fires when the
//    stored token changes externally, so live MCP sessions can tear down
//    and rebuild with fresh creds.
//
// Thread-safe: all public methods take the manager's mutex.  Reconnect
// callbacks are invoked with the mutex released so they can call back into
// the manager without deadlocking.
// ----------------------------------------------------------------------------

// Outcome of ``handle_401`` — lets the caller decide whether to retry the
// failing tool call, trigger a fresh browser flow, or bubble a needs-reauth
// error up to the model.
enum class McpOAuthRecovery {
    kNoCredentials,   // never logged in; user must run `hermes mcp login`
    kRefreshedOnDisk, // disk mtime moved — caller should retry with the
                      // reloaded token
    kRefreshedInPlace,// we successfully issued a refresh_token exchange
    kNeedsRelogin,    // refresh_token is invalid / 401-forced relogin
    kTransient,       // 5xx / network — retry later
};

class MCPOAuthManager {
public:
    using ReconnectCallback = std::function<void(const std::string& server)>;

    // Factory-style ctor takes the HTTP transport that the per-server
    // McpOAuth instances will use.  Passing nullptr falls back to the
    // process-global default transport.
    explicit MCPOAuthManager(hermes::llm::HttpTransport* transport = nullptr);

    // Out-of-line destructor — required because ``ServerState`` is only
    // forward-declared here and the default unique_ptr deleter needs the
    // complete type.
    ~MCPOAuthManager();

    MCPOAuthManager(const MCPOAuthManager&) = delete;
    MCPOAuthManager& operator=(const MCPOAuthManager&) = delete;

    // Register/deregister a callback invoked when the token for ``server``
    // changes due to an external refresh (mtime moved) OR an in-place
    // refresh this manager performed.  Multiple callbacks per server are
    // supported — they fire in registration order.  Returns an opaque
    // handle that can be passed to ``unsubscribe_reconnect``.
    std::size_t subscribe_reconnect(const std::string& server,
                                    ReconnectCallback cb);
    void unsubscribe_reconnect(const std::string& server, std::size_t handle);

    // Get the cached token for ``server``.  Loads from disk on first
    // access, and reloads if the file mtime has advanced since the last
    // read (mtime watch).  Returns nullopt when the file doesn't exist.
    std::optional<McpOAuthToken> get_token(const std::string& server);

    // Called when an MCP tool call / transport request gets a 401.  The
    // manager checks if the on-disk token has been externally refreshed;
    // if yes, reloads and fires the reconnect callback.  Otherwise attempts
    // an in-place refresh via ``McpOAuth::refresh``.  Callers receive an
    // ``McpOAuthRecovery`` enum they can switch on.  Concurrent calls for
    // the same server dedupe into a single refresh attempt.
    McpOAuthRecovery handle_401(const std::string& server,
                                const McpOAuthConfig& cfg);

    // Explicit reflow: wipes the on-disk token file and the in-memory
    // cache, runs the interactive login, persists the result, and fires
    // the reconnect callback so live sessions pick up the new creds.
    // Intended target for ``hermes mcp login <name>``.
    std::optional<McpOAuthToken> relogin(const std::string& server,
                                         const McpOAuthConfig& cfg);

    // Drop the in-memory cache entry for ``server`` without touching disk.
    // Used by ``hermes mcp remove``.
    void invalidate(const std::string& server);

    // Test/introspection hooks -----------------------------------------

    // Peek the currently-cached token without checking mtime.  Returns
    // nullopt if the server has no cached entry.
    std::optional<McpOAuthToken> peek_cached(const std::string& server) const;

    // Number of live reconnect subscribers for ``server``.
    std::size_t subscriber_count(const std::string& server) const;

private:
    struct ServerState;

    hermes::llm::HttpTransport* transport_;
    mutable std::mutex mu_;
    std::unordered_map<std::string, std::unique_ptr<ServerState>> servers_;
    std::unordered_set<std::string> inflight_;
    std::condition_variable inflight_cv_;

    ServerState& ensure_state_(const std::string& server);
    // Reload the token from disk if the file mtime has advanced.  Returns
    // true if a reload occurred and the callback should fire.  Must be
    // called with mu_ held; the callback must be invoked with mu_ released.
    bool reload_if_changed_(ServerState& st);
    void fire_reconnect_unlocked_(const std::string& server);
};

}  // namespace hermes::auth
