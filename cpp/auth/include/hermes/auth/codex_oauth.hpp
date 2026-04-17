// Codex (ChatGPT) OAuth credentials loader.
//
// Reads ``~/.codex/auth.json`` (or ``$CODEX_HOME/auth.json``) which is
// maintained by the OpenAI Codex CLI.  Hermes reuses the same on-disk
// token so that users who have already logged into Codex do not need
// to re-authenticate to Hermes — they just pick ``provider:
// openai-codex`` and we piggy-back on Codex's access_token.
//
// Refresh is delegated to the Codex CLI itself: if the access_token is
// expired, the user runs ``codex`` (or any Codex invocation) to
// refresh, and Hermes picks up the new token on the next call.  This
// avoids duplicating OpenAI's ChatGPT-account OAuth flow on our side.
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
std::optional<CodexCredentials> load_codex_credentials();

// Same as load_codex_credentials() but reads from an explicit path.
// Visible for tests.
std::optional<CodexCredentials> load_codex_credentials_from(
    const std::filesystem::path& auth_json_path);

}  // namespace hermes::auth
