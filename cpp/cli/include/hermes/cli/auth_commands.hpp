// C++17 port of hermes_cli/auth_commands.py — credential-pool auth
// subcommand helpers and display formatters.
//
// Orchestration (device-code login, pool I/O) lives in hermes::auth; this
// module carries only the UI/format layer plus a thin command-args struct
// that the CLI dispatcher can populate.
#pragma once

#include <nlohmann/json.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace hermes::cli::auth_commands {

// Providers whose canonical auth flow is OAuth (in addition to API keys).
// Exposed so the interactive picker can branch correctly.
const std::vector<std::string>& oauth_capable_providers();

// Credential-pool auth-type constants — mirror agent/credential_pool.py.
extern const char* const AUTH_TYPE_API_KEY;
extern const char* const AUTH_TYPE_OAUTH;
extern const char* const SOURCE_MANUAL;
extern const char* const STATUS_EXHAUSTED;
extern const char* const CUSTOM_POOL_PREFIX;

// Strategy names — used for config round-trips.
extern const char* const STRATEGY_FILL_FIRST;
extern const char* const STRATEGY_ROUND_ROBIN;
extern const char* const STRATEGY_LEAST_USED;
extern const char* const STRATEGY_RANDOM;

// Normalise a provider name typed at the CLI:
//  * trim + lower-case
//  * fold "or" / "open-router" → "openrouter"
//  * resolve custom-providers display name → "custom:<slug>"
// *custom_providers* is the ``custom_providers:`` list from config.yaml
// (may be null).
std::string normalize_provider(const std::string& raw,
                               const nlohmann::json& custom_providers = {});

// If *raw* matches a custom_providers entry (case-insensitive), return the
// corresponding pool key ("custom:<slug>").  Empty when no match.
std::string resolve_custom_provider_input(const std::string& raw,
                                          const nlohmann::json& custom_providers);

// Canonicalise the custom-pool suffix (same rules as Python's
// _normalize_custom_pool_name: trim, lower-case, replace spaces with
// hyphens).
std::string normalize_custom_pool_name(const std::string& display_name);

// Default label generators, matching the Python versions.
std::string oauth_default_label(const std::string& provider, std::size_t count);
std::string api_key_default_label(std::size_t count);

// Strip the "manual:" prefix from a source string for display.
std::string display_source(const std::string& source);

// Return the inference base URL for a provider — either the hermes
// overlay's override or a custom-provider entry's `base_url`.
std::string provider_base_url(const std::string& provider,
                              const nlohmann::json& custom_providers = {});

// Credential entry as emitted by the pool loader.  Mirrors the Python
// PooledCredential dataclass to the extent the formatters care about.
struct PooledEntry {
    std::string provider;
    std::string id;
    std::string label;
    std::string auth_type;
    std::string source;
    std::string last_status;
    std::string last_error_reason;
    std::string last_error_code;
    std::optional<double> exhausted_until;  // unix epoch seconds
};

// Format a one-line trailing status suffix ("exhausted (2m 30s left)" etc).
// Returns empty when the entry isn't exhausted.
std::string format_exhausted_status(const PooledEntry& entry, double now_epoch);

// Render a table for `hermes auth list` (single provider).  Emits to a
// string so tests can assert on the full output.
std::string render_list_table(const std::string& provider,
                              const std::vector<PooledEntry>& entries,
                              const std::optional<std::string>& current_id,
                              double now_epoch);

// -- Command input structs --------------------------------------------------
// These mirror the argparse namespaces used by the Python callbacks.  The
// actual pool-writing logic is outside this module's scope (it needs
// hermes::auth); the structs let us fully parse + validate the args here.

struct AuthAddArgs {
    std::string provider;              // raw, pre-normalisation
    std::string auth_type;             // "api_key" | "oauth" | ""
    std::string api_key;               // when blank, caller must prompt
    std::string label;
    // OAuth-specific knobs (for Nous device-code flow).
    std::string portal_url;
    std::string inference_url;
    std::string client_id;
    std::string scope;
    bool no_browser = false;
    std::optional<double> timeout;
    bool insecure = false;
    std::string ca_bundle;
    int min_key_ttl_seconds = 300;  // 5 min default
};

struct AuthRemoveArgs {
    std::string provider;
    std::string target;  // id | label | "#N" | index
};

struct AuthResetArgs {
    std::string provider;
};

// Resolve the requested auth_type, applying the Python defaults:
//   * api-key providers → AUTH_TYPE_API_KEY
//   * oauth-capable providers → AUTH_TYPE_OAUTH
// Empty strings are replaced.  Unknown values pass through unchanged so
// the caller can raise.
std::string resolve_auth_type(const std::string& requested,
                              const std::string& canonical_provider);

// Validate a provider name against the known set (overlays + openrouter +
// custom prefix) — returns empty string on success, error message on
// failure.
std::string validate_provider_choice(const std::string& canonical_provider);

// Build a helpful summary of which env var holds the API key for a given
// provider — used by the `auth add` interactive path to point users at
// the dashboard pages.
std::string env_var_guidance(const std::string& canonical_provider);

// Rendering helpers for the interactive picker.
std::string render_provider_picker_hint(
    const std::vector<std::string>& known_providers,
    const std::vector<std::string>& custom_display_names);

// Strategy helpers for the `set rotation strategy` interactive path.
struct StrategyChoice {
    std::string name;
    std::string description;
};
const std::vector<StrategyChoice>& strategy_catalog();

// Apply a user-chosen strategy index to the supplied config JSON (mutates
// the `credential_pool_strategies` map).  Returns true on success, false
// on an out-of-range index.
bool apply_strategy_choice(nlohmann::json& config,
                           const std::string& provider,
                           std::size_t choice_index);

// Read the current strategy for *provider* from config, falling back to
// STRATEGY_FILL_FIRST.
std::string read_strategy(const nlohmann::json& config,
                          const std::string& provider);

}  // namespace hermes::cli::auth_commands
