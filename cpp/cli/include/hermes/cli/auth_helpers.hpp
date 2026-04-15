// C++17 port of provider-specific helpers from `hermes_cli/auth.py` that
// layer on top of `auth_core`.
//
// Scope:
//   * Provider base-URL routing rules (Kimi, Z.AI, Codex, Qwen).
//   * GitHub CLI binary discovery (`_gh_cli_candidates`).
//   * Placeholder-secret denylist (extends `has_usable_secret`).
//   * Qwen CLI on-disk credential helpers (`~/.qwen/oauth_creds.json`).
//   * Qwen access-token expiry check.
//   * Z.AI endpoint candidate table (used by `detect_zai_endpoint`).
//   * SHA-256 short fingerprint used by Z.AI cache key.
//
// HTTPS calls (token refresh, endpoint probing, device-code polling) are
// intentionally left out — they require the global HTTPS client wiring.
#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

namespace hermes::cli::auth_helpers {

// ---------------------------------------------------------------------------
// Constants — match the Python module verbatim.
// ---------------------------------------------------------------------------

inline constexpr const char* kKimiCodeBaseUrl =
    "https://api.kimi.com/coding/v1";
inline constexpr const char* kDefaultCodexBaseUrl =
    "https://chatgpt.com/backend-api/codex";
inline constexpr const char* kDefaultQwenBaseUrl = "https://portal.qwen.ai/v1";
inline constexpr const char* kDefaultGithubModelsBaseUrl =
    "https://api.githubcopilot.com";
inline constexpr const char* kDefaultCopilotAcpBaseUrl = "acp://copilot";
inline constexpr const char* kCodexOauthClientId =
    "app_EMoamEEZ73f0CkXaXp7hrann";
inline constexpr const char* kCodexOauthTokenUrl =
    "https://auth.openai.com/oauth/token";
inline constexpr int kCodexAccessTokenRefreshSkewSeconds = 120;
inline constexpr const char* kQwenOauthClientId =
    "f0304373b74a44d2b584a3fb70ca9e56";
inline constexpr const char* kQwenOauthTokenUrl =
    "https://chat.qwen.ai/api/v1/oauth2/token";
inline constexpr int kQwenAccessTokenRefreshSkewSeconds = 120;

// ---------------------------------------------------------------------------
// Placeholder secret denylist.
// ---------------------------------------------------------------------------

// Returns the lower-case set of values that are treated as placeholder
// secrets ("changeme", "your_api_key", "*", ...).  Mirrors
// `_PLACEHOLDER_SECRET_VALUES`.
const std::unordered_set<std::string>& placeholder_secret_values();

// True when `value` is non-empty, ≥ `min_length` chars, and not a known
// placeholder.  Mirrors the Python `has_usable_secret` semantics
// including the placeholder denylist (in addition to the basic length
// check exposed by `auth_core::has_usable_secret`).
bool has_usable_secret(const std::string& value, std::size_t min_length = 4);

// ---------------------------------------------------------------------------
// Provider base-URL routing.
// ---------------------------------------------------------------------------

// Kimi: `sk-kimi-`-prefixed keys route to the coding-plan endpoint.
// `env_override` always wins.  Mirrors `_resolve_kimi_base_url`.
std::string resolve_kimi_base_url(const std::string& api_key,
                                  const std::string& default_url,
                                  const std::string& env_override);

// ---------------------------------------------------------------------------
// Z.AI endpoint table + cache-key helpers.
// ---------------------------------------------------------------------------

struct ZaiEndpoint {
    std::string id;
    std::string base_url;
    std::string default_model;
    std::string label;
};

const std::vector<ZaiEndpoint>& zai_endpoints();

// SHA-256 of `api_key`, hex-encoded, truncated to 16 chars.  Used as
// the cache key in `auth.json` Z.AI provider state.
std::string zai_key_hash(const std::string& api_key);

// ---------------------------------------------------------------------------
// GitHub CLI discovery.
// ---------------------------------------------------------------------------

// Returns candidate `gh` binary paths in the order Python would try
// them: PATH first, then common Homebrew + ~/.local/bin locations.
// Each returned path is verified to exist + be executable.
std::vector<std::string> gh_cli_candidates();

// ---------------------------------------------------------------------------
// JWT helpers.
// ---------------------------------------------------------------------------

// Decode a JWT payload segment.  Returns an empty object on any
// failure (malformed structure, invalid base64, non-JSON payload, or
// wrong segment count).  Mirrors `_decode_jwt_claims`.
nlohmann::json decode_jwt_claims(const std::string& token);

// True when a Codex JWT's `exp` claim falls within the skew window.
// Missing / non-numeric `exp` → false (no force-refresh).  Negative
// skew is clamped to zero.  Mirrors `_codex_access_token_is_expiring`.
bool codex_access_token_is_expiring(const std::string& access_token,
                                    int skew_seconds);

// True when the Qwen `expiry_date` (millis since epoch) has lapsed or
// will within `skew_seconds`.  Non-numeric → true (force-refresh).
// Mirrors `_qwen_access_token_is_expiring`.
bool qwen_access_token_is_expiring(
    const nlohmann::json& expiry_date_ms,
    int skew_seconds = kQwenAccessTokenRefreshSkewSeconds);

// ---------------------------------------------------------------------------
// Qwen CLI credential file (~/.qwen/oauth_creds.json).
// ---------------------------------------------------------------------------

std::filesystem::path qwen_cli_auth_path();

// Read + JSON-parse the Qwen CLI credentials file.  Throws
// `auth_core::AuthError` (provider="qwen-oauth") on missing file,
// I/O failure, parse failure, or non-object root.
nlohmann::json read_qwen_cli_tokens();

// Atomic write — `oauth_creds.json` is written via `oauth_creds.tmp`
// then renamed.  Permissions are tightened to 0600.  Returns the path.
std::filesystem::path save_qwen_cli_tokens(const nlohmann::json& tokens);

}  // namespace hermes::cli::auth_helpers
