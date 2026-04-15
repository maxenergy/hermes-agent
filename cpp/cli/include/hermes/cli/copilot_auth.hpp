// C++17 port of the pure-logic helpers from
// `hermes_cli/copilot_auth.py`.
//
// Scope of the port:
//   * Token-prefix validation (`validate_copilot_token`).
//   * Environment-variable lookup order used by
//     `resolve_copilot_token` (env-only fast path; the `gh auth token`
//     subprocess fallback is left to the C++ caller who already owns a
//     process runner).
//   * `gh` binary candidate enumeration
//     (`gh_cli_candidates`).
//   * Request-header construction (`copilot_request_headers`).
//   * Device-code OAuth polling *state machine*
//     (`classify_device_code_response`,
//     `compute_next_poll_interval`) -- the network IO is isolated in a
//     testable helper so tests can drive the flow without HTTP.
//
// Constants mirror the Python module byte-for-byte.
#pragma once

#include <array>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace hermes::cli::copilot_auth {

// OAuth device-code client ID (shared with opencode/Copilot CLI).
inline constexpr const char* k_oauth_client_id = "Ov23li8tweQw6odWQebz";

// Classic PAT prefix -- NOT supported by the Copilot API.
inline constexpr const char* k_classic_pat_prefix = "ghp_";

// Supported token prefixes.  `validate_copilot_token` accepts any of
// these.  (The Python module lists them but only special-cases the
// classic PAT; we expose both for introspection.)
inline constexpr std::array<const char*, 3> k_supported_prefixes{
    "gho_", "github_pat_", "ghu_",
};

// Environment variables consulted when resolving a token, in priority
// order.  Mirrors `COPILOT_ENV_VARS`.
inline constexpr std::array<const char*, 3> k_env_vars{
    "COPILOT_GITHUB_TOKEN", "GH_TOKEN", "GITHUB_TOKEN",
};

// Device-code polling constants (seconds).
inline constexpr int k_device_code_poll_interval = 5;
inline constexpr int k_device_code_poll_safety_margin = 3;

// Result of `validate_copilot_token`.
struct token_validation_result {
    bool valid{false};
    std::string message{};
};

// Validate a token against the Copilot API's prefix requirements.
// Returns `{false, <error>}` for an empty token or a classic PAT
// (`ghp_*`); `{true, "OK"}` otherwise.
token_validation_result validate_copilot_token(const std::string& token);

// Functor injected by `resolve_copilot_token_from_env` so tests can
// avoid reading the real environment.  Receives the env-var name and
// returns the raw value, or empty string if unset.
using env_lookup_fn = std::string (*)(const char*);

// Best-effort token resolution from the three environment variables
// listed in `k_env_vars`.  Returns `{token, env_var_name}` for the
// first supported value (after prefix validation) or empty strings if
// none is available.  Unsupported tokens are skipped silently -- the
// caller is expected to fall back to `gh auth token`.
struct token_resolution {
    std::string token{};
    std::string source{};
};

token_resolution resolve_copilot_token_from_env(env_lookup_fn lookup = nullptr);

// Return an ordered list of candidate `gh` binary paths.
//
//   1. `which("gh")` if present (expressed via `resolved_path`).
//   2. `/opt/homebrew/bin/gh`
//   3. `/usr/local/bin/gh`
//   4. `$HOME/.local/bin/gh`
//
// Duplicates are suppressed.  The `executable_check` callback receives
// a candidate path and must return `true` if the path exists and is
// executable (tests pass a table-driven stub; production wires this up
// to `access(X_OK)`).
using executable_check_fn = bool (*)(const std::string&);

std::vector<std::string> gh_cli_candidates(
    const std::string& resolved_path,
    const std::string& home_dir,
    executable_check_fn executable_check);

// Build the standard Copilot API request headers.  Order is
// deterministic to simplify test assertions.
std::unordered_map<std::string, std::string> copilot_request_headers(
    bool is_agent_turn = true,
    bool is_vision = false);

// Classification of a device-code poll response (the JSON payload
// returned by `POST /login/oauth/access_token`).  Mirrors the branches
// in `copilot_device_code_login`.
enum class device_code_status {
    success,
    pending,
    slow_down,
    expired,
    access_denied,
    error,
};

struct device_code_poll_decision {
    device_code_status status{device_code_status::error};
    // Populated for `success`.
    std::string access_token{};
    // Populated for `error` or when we want to surface a server error.
    std::string error_message{};
};

// Classify a poll response body.  The response is the parsed JSON from
// the OAuth endpoint represented as key/value pairs; both
// `access_token` and `error` are examined.
device_code_poll_decision classify_device_code_response(
    const std::unordered_map<std::string, std::string>& response);

// Compute the next polling interval after a `slow_down` response per
// RFC 8628.  If the server supplied a positive `interval` value it
// wins; otherwise we add 5 seconds to the previous interval.
//
// `server_interval_str` is the raw string value (or empty if absent);
// parsing rules match the Python `isinstance(..., (int, float)) and > 0`
// guard.
int compute_next_poll_interval(int current_interval,
                               const std::string& server_interval_str);

}  // namespace hermes::cli::copilot_auth
