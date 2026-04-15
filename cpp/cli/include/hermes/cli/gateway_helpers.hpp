// C++17 port of pure-logic helpers from `hermes_cli/gateway.py` that
// drive `hermes gateway install` / `hermes gateway start` flows.
//
// Scope:
//   * `_profile_suffix` — derive a service-name suffix from HERMES_HOME.
//   * `_profile_arg`    — render `--profile <name>` for service ExecStart.
//   * `get_service_name`  — `hermes-gateway[-<suffix>]`.
//   * Path remapping: `_remap_path_for_user`, `_hermes_home_for_target_user`,
//     `_build_user_local_paths`.
//   * Service-definition normaliser (`_normalize_service_definition`).
//   * launchd label derivation.
//   * Restart drain timeout parsing.
//
// All filesystem logic uses pure path math — no actual home-dir lookup
// — so callers can pass arbitrary paths from tests without touching
// the real filesystem.
#pragma once

#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace hermes::cli::gateway_helpers {

// ---------------------------------------------------------------------------
// Constants.
// ---------------------------------------------------------------------------

inline constexpr const char* kServiceBase = "hermes-gateway";
inline constexpr const char* kLaunchdLabelBase = "ai.nous.hermes.gateway";
inline constexpr int kDefaultRestartDrainSeconds = 60;
inline constexpr int kGatewayServiceRestartExitCode = 75;

// ---------------------------------------------------------------------------
// Profile-suffix derivation.
// ---------------------------------------------------------------------------

// Derive the per-profile suffix used in service / launchd labels.
//
//   * `hermes_home == default_root` → ""
//   * `hermes_home == default_root/profiles/<name>` and `<name>` matches
//     `[a-z0-9][a-z0-9_-]{0,63}` → `<name>`
//   * Anything else → first 8 hex chars of SHA-256(hermes_home).
//
// `default_root` is usually `~/.hermes`, but is passed explicitly so the
// helper stays free of HOME lookup side-effects (mirrors Python's
// `get_default_hermes_root`).
std::string profile_suffix(const std::filesystem::path& hermes_home,
                           const std::filesystem::path& default_root);

// True when the suffix is a valid bare profile name (lower-case
// alphanumeric + dash/underscore, ≤ 64 chars).
bool is_valid_profile_name(const std::string& name);

// Render `--profile <name>` only when the profile is a valid named
// profile.  Returns "" otherwise (default profile, custom path, hash).
std::string profile_arg(const std::filesystem::path& hermes_home,
                        const std::filesystem::path& default_root);

// `hermes-gateway[-<suffix>]` — concatenation of `kServiceBase` and the
// suffix (when non-empty).
std::string service_name(const std::filesystem::path& hermes_home,
                         const std::filesystem::path& default_root);

// `ai.nous.hermes.gateway[.<suffix>]`.
std::string launchd_label(const std::filesystem::path& hermes_home,
                          const std::filesystem::path& default_root);

// Path to the user-scoped systemd unit file:
//   `<config_home>/systemd/user/<service_name>.service`.
std::filesystem::path user_systemd_unit_path(
    const std::filesystem::path& config_home,
    const std::filesystem::path& hermes_home,
    const std::filesystem::path& default_root);

// Path to the system-scoped systemd unit file:
//   `/etc/systemd/system/<service_name>.service`.
std::filesystem::path system_systemd_unit_path(
    const std::filesystem::path& hermes_home,
    const std::filesystem::path& default_root);

// ---------------------------------------------------------------------------
// Path remapping for cross-user system-service installs.
// ---------------------------------------------------------------------------

// If `path` lives under `current_home`, rewrite the prefix to
// `target_home`.  Otherwise return `path` unchanged.  Mirrors
// `_remap_path_for_user`.
std::string remap_path_for_user(const std::string& path,
                                const std::filesystem::path& current_home,
                                const std::filesystem::path& target_home);

// Translate the current HERMES_HOME to the equivalent path under
// `target_home`:
//   * `current_home/.hermes` → `target_home/.hermes`
//   * profile sub-dir under that → preserve the relative structure
//   * anything else → return `current_hermes` unchanged.
// Mirrors `_hermes_home_for_target_user`.
std::filesystem::path hermes_home_for_target_user(
    const std::filesystem::path& current_hermes,
    const std::filesystem::path& current_home,
    const std::filesystem::path& target_home);

// Returns the user-local bin dirs (`~/.local/bin`, `~/.cargo/bin`,
// `~/go/bin`, `~/.npm-global/bin`) that exist AND aren't already in
// `path_entries`.  `existence_check` is injected so tests can run
// without relying on the real filesystem; when null defaults to
// `std::filesystem::exists`.
using ExistenceCheck = std::function<bool(const std::filesystem::path&)>;
std::vector<std::string> build_user_local_paths(
    const std::filesystem::path& home,
    const std::vector<std::string>& path_entries,
    const ExistenceCheck& existence_check = {});

// ---------------------------------------------------------------------------
// Service-definition canonicaliser.
// ---------------------------------------------------------------------------

// Strip trailing whitespace from each line and surrounding blank
// lines.  Mirrors `_normalize_service_definition`.  Used to compare
// installed unit files against freshly-generated ones.
std::string normalize_service_definition(const std::string& text);

// ---------------------------------------------------------------------------
// Restart-drain timeout parsing.
// ---------------------------------------------------------------------------

// Parse a positive-integer-seconds string (or empty / invalid).
// Returns nullopt on parse failure or non-positive values.  Mirrors
// `parse_restart_drain_timeout`.
std::optional<int> parse_restart_drain_timeout(const std::string& raw);

// ---------------------------------------------------------------------------
// Discord allowlist parsing — used by the gateway setup wizard.
// ---------------------------------------------------------------------------

// Split a comma-separated allowlist into trimmed user-id strings;
// drops blanks.  Mirrors the inline parsing logic used by
// `_setup_discord` after `_clean_discord_user_ids`.
std::vector<std::string> split_allowlist(const std::string& csv);

}  // namespace hermes::cli::gateway_helpers
