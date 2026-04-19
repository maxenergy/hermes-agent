// Codex (ChatGPT) OAuth credentials loader.
//
// Reads ``~/.codex/auth.json`` (or ``$CODEX_HOME/auth.json``).  This
// file is maintained by the OpenAI Codex CLI / VS Code extension and
// is NOT Hermes's runtime credential store — Hermes owns its own
// Codex auth state in ``~/.hermes/auth.json`` / ``~/.hermes/.env``.
//
// OpenAI OAuth refresh tokens are single-use and rotate on every
// refresh; if Hermes and the Codex CLI shared the same on-disk file,
// whoever refreshed last would invalidate the other side's token.
// Upstream Python commit b02833f3 stopped touching this path at
// runtime for exactly that reason.
//
// This loader therefore exists only for the one-time, user-gated
// import performed by ``hermes auth openai-codex`` when the user
// has previously logged in via the Codex CLI and wants to seed the
// Hermes auth store without going through OAuth again.  Do NOT call
// it from hot paths (provider resolution, request loops, refresh
// callbacks).  Writes to ``~/.codex/auth.json`` are not supported
// and must never be added.
#pragma once

#include <chrono>
#include <filesystem>
#include <optional>
#include <string>

namespace hermes::auth {

struct CodexCredentials {
    // "chatgpt" (OAuth flow) or "apikey" (user-provided OPENAI_API_KEY).
    std::string auth_mode;
    // JWT access token (Bearer).  Empty when auth_mode=="apikey" and
    // OPENAI_API_KEY is injected via env by the CLI.
    std::string access_token;
    std::string id_token;
    std::string refresh_token;
    // ChatGPT account UUID — must be passed in the ``chatgpt-account-id``
    // header when hitting ``chatgpt.com/backend-api/codex``.
    std::string account_id;
    // Optional plaintext API key (only populated when auth_mode=="apikey").
    std::string api_key;
    // Last successful refresh time (ISO-8601 in the source file).
    std::chrono::system_clock::time_point last_refresh{};
};

// Resolved directory: ``$CODEX_HOME`` if set and non-empty, else
// ``$HOME/.codex``.  May not exist.
std::filesystem::path codex_home();

// Load credentials from ``codex_home() / "auth.json"``.  Returns
// nullopt if the file is missing, unreadable, or has no recognisable
// fields.  Partial payloads are accepted — fields the caller doesn't
// need may be empty.
//
// ONE-TIME-IMPORT ONLY.  See the file header comment: this must not
// be used as a runtime fallback for token resolution.  The intended
// caller is the ``hermes auth openai-codex`` setup flow seeding the
// Hermes auth store from a pre-existing Codex CLI login.
std::optional<CodexCredentials> load_codex_credentials();

// Same as load_codex_credentials() but reads from an explicit path.
// Visible for tests.
std::optional<CodexCredentials> load_codex_credentials_from(
    const std::filesystem::path& auth_json_path);

// ----- Refresh-response classifier ------------------------------------------
//
// Hermes currently reaches Codex through CLIProxyAPI, which owns the OAuth
// refresh loop on our behalf.  When we wire up a native direct refresh
// path against ``https://auth.openai.com/oauth/token``, call sites must
// classify the HTTP response through ``classify_codex_refresh_response``
// and propagate ``relogin_required`` into AuthError — otherwise users see
// bare HTTP failures with no guidance to re-authenticate.
//
// Ports upstream Python commit ``2a2e5c0f`` (``hermes_cli/auth.py``):
// any 401/403 from the token endpoint is treated as invalid creds and
// forces a relogin even when the JSON body lacks a recognised error
// code.  500+ stays transient.
struct CodexRefreshClassification {
    bool relogin_required = false;   // user must rerun `hermes auth openai-codex`
    bool transient = false;          // 5xx / network; safe to retry
    std::string error_code;          // e.g. "invalid_grant", "access_denied"; empty on unknown
    std::string message;             // human-readable guidance
};

CodexRefreshClassification classify_codex_refresh_response(
    int status_code,
    const std::string& response_body);

}  // namespace hermes::auth
