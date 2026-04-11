// High-level config load/save pipeline for Hermes.
//
// Responsibilities:
//  - Locate the active config file (`<HERMES_HOME>/config.yaml`).
//  - Deep-merge the user's overlay on top of `default_config()`.
//  - Expand `${VAR}` / `${VAR:-default}` references in string values.
//  - Write back atomically via `hermes::core::atomic_write`.
//  - Detect the host "managed system" (NixOS / Homebrew / Debian).
//  - Run schema migrations between config versions.
#pragma once

#include <nlohmann/json.hpp>
#include <string>
#include <string_view>

namespace hermes::config {

// Load config for the CLI path (`hermes ...`). Reads
// `<HERMES_HOME>/config.yaml`, merges over `default_config()`, and
// expands environment variables in string values.  Missing files are
// treated as an empty overlay — the defaults are still returned.
nlohmann::json load_cli_config();

// Load config for the `hermes tools` / `hermes setup` path. Behaves
// identically to `load_cli_config()` for now — the distinction is
// preserved so future phases can diverge (e.g. different migration
// policies) without touching call sites.
nlohmann::json load_config();

// Serialise `config` to YAML and write it atomically to
// `<HERMES_HOME>/config.yaml`.  Parent directories are created if they
// do not already exist.  Throws `std::runtime_error` on IO failure.
void save_config(const nlohmann::json& config);

// Expand `${VAR}` and `${VAR:-default}` references in `input`.  No
// nested expansion.  Literal `$$` is *not* collapsed (we leave it
// as-is); unresolved `${UNSET}` stays verbatim so callers can detect
// missing values.  Only braced forms are expanded — bare `$VAR` is a
// literal.
std::string expand_env_vars(std::string_view input);

enum class ManagedSystem {
    None,
    NixOS,
    Homebrew,
    Debian,
};

// Best-effort detection of how Hermes is installed on the host.  Reads
// `/etc/os-release` plus a few filesystem probes (`/nix`,
// `/opt/homebrew`).  Never throws.
ManagedSystem detect_managed_system();

// Run schema migrations on `config`, bumping `_config_version` in the
// process.  Returns the migrated config.  Phase 1 only bumps the
// version stamp; real per-field migrations are TODO.
nlohmann::json migrate_config(nlohmann::json config);

}  // namespace hermes::config
