// Google Gemini CLI OAuth — PKCE Authorization Code flow.
//
// Ports upstream Python commit 3524ccfc (agent/google_oauth.py) which talks
// to Google's Code Assist backend (cloudcode-pa.googleapis.com) using the
// public gemini-cli desktop OAuth client.  The resulting access token powers
// both the free tier (personal accounts) and paid tiers (GCP projects).
//
// Endpoints:
//   authorize: https://accounts.google.com/o/oauth2/v2/auth
//   token:     https://oauth2.googleapis.com/token
//   userinfo:  https://www.googleapis.com/oauth2/v1/userinfo
//
// Storage: ``$HERMES_HOME/auth/google_oauth.json`` (mode 0600).  Mirrors the
// Python on-disk layout exactly — the ``refresh`` field packs the refresh
// token together with the resolved GCP project IDs so subsequent sessions
// don't need to re-discover the project:
//
//   {
//     "refresh": "<refresh_token>|<project_id>|<managed_project_id>",
//     "access":  "...",
//     "expires": 1744848000000,   // unix MILLIseconds
//     "email":   "user@example.com"
//   }
//
// Google's public gemini-cli desktop OAuth client is NOT confidential: desktop
// clients use PKCE for security, not client_secret.  This matches Google's own
// open-source gemini-cli distribution.  Users can override via the
// ``HERMES_GEMINI_CLIENT_ID`` / ``HERMES_GEMINI_CLIENT_SECRET`` env vars.
#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

namespace hermes::llm {
class HttpTransport;
}

namespace hermes::auth {

struct GeminiCredentials {
    std::string access_token;
    std::string refresh_token;
    std::string token_type = "Bearer";
    int64_t expiry_date_ms = 0;     // unix milliseconds
    std::string email;              // resolved from userinfo (best-effort)
    std::string project_id;         // resolved GCP project (optional)
    std::string managed_project_id; // Code Assist managed project (optional)

    // Free-tier detection: both project IDs empty after onboarding means the
    // user is on Google's personal-account free tier; paid tiers populate
    // project_id from a configured GCP project.  The upstream Python uses the
    // Code Assist loadCodeAssist response to confirm, but a missing project_id
    // is a strong indicator on its own.
    bool is_free_tier() const { return project_id.empty(); }

    bool empty() const { return access_token.empty(); }
    bool expired(int64_t now_ms) const {
        return expiry_date_ms != 0 && now_ms >= expiry_date_ms;
    }
    // Upstream keeps a 60 s skew — refresh before the token expires.
    bool needs_refresh(int64_t now_ms, int64_t margin_ms = 60'000) const {
        return expiry_date_ms != 0 && now_ms + margin_ms >= expiry_date_ms;
    }
};

// Atomic 0600 JSON store for Gemini credentials.  Default path is
// ``$HERMES_HOME/auth/google_oauth.json`` (matches upstream).
class GeminiCredentialStore {
public:
    GeminiCredentialStore();
    explicit GeminiCredentialStore(std::filesystem::path path);

    GeminiCredentials load() const;
    bool save(const GeminiCredentials& creds) const;
    bool clear() const;

    const std::filesystem::path& path() const { return path_; }

private:
    std::filesystem::path path_;
};

// PKCE pair (verifier + S256 challenge).  Exposed for tests.
struct GeminiPkcePair {
    std::string verifier;
    std::string challenge_s256;
};

class GeminiOAuth {
public:
    explicit GeminiOAuth(hermes::llm::HttpTransport* transport = nullptr);

    // Configuration overrides — if empty the ctor pulls from
    // HERMES_GEMINI_CLIENT_ID / HERMES_GEMINI_CLIENT_SECRET env vars, falling
    // back to the public gemini-cli desktop OAuth client.
    void set_client_id(std::string id);
    void set_client_secret(std::string secret);
    const std::string& client_id() const { return client_id_; }
    const std::string& client_secret() const { return client_secret_; }

    // PKCE S256 pair.  Test hook.
    static GeminiPkcePair generate_pkce();

    // Exchange an authorization code (captured by the local callback server)
    // for access + refresh tokens.  Throws on HTTP error.
    std::optional<GeminiCredentials> exchange_code(
        const std::string& code,
        const std::string& code_verifier,
        const std::string& redirect_uri);

    // Refresh the access token.  Returns nullopt on permanent failure
    // (invalid_grant, 401/403) — caller must rerun interactive_login.
    // Any 401/403 is classified via classify_codex_refresh_response semantics
    // (see hermes/auth/codex_oauth.hpp) so users see consistent relogin
    // guidance across providers.
    std::optional<GeminiCredentials> refresh(const GeminiCredentials& current);

    // End-to-end interactive browser flow.  Spawns a local HTTP listener on a
    // loopback port, opens the browser to Google's authorize endpoint, waits
    // for the ?code= callback, exchanges the code, and persists to the store.
    // Headless environments (SSH_CONNECTION / HERMES_HEADLESS) fall through
    // to the paste-mode variant with a manual prompt.
    std::optional<GeminiCredentials> interactive_login(GeminiCredentialStore& store);

    // Get a guaranteed-fresh token, refreshing if within the skew margin.
    // Returns nullopt if no credentials are stored.  On a permanent refresh
    // failure the store is cleared.
    std::optional<GeminiCredentials> ensure_valid(GeminiCredentialStore& store);

private:
    hermes::llm::HttpTransport* transport_;
    std::string client_id_;
    std::string client_secret_;

    // Try to exchange packed refresh payload with Google, preserving
    // project_id / managed_project_id across refreshes when Google omits them
    // from the token response.
    void merge_refresh_response(const std::string& body,
                                GeminiCredentials& out,
                                const GeminiCredentials& previous);
};

}  // namespace hermes::auth
