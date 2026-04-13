// Phase 12: OAuth 2.1 + PKCE authorization-code flow for MCP servers.
//
// Spawns a transient localhost HTTP listener on ``redirect_uri``, opens the
// user's browser, waits for the ``?code=`` callback, and exchanges it at
// the token endpoint.  Tokens persist under
// ``$HERMES_HOME/mcp/<server_name>.token.json`` (mode 0600).
#pragma once

#include <cstdint>
#include <optional>
#include <string>
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

}  // namespace hermes::auth
