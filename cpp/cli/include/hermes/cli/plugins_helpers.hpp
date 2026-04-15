// C++17 port of pure-logic helpers from `hermes_cli/plugins_cmd.py` that
// drive `hermes plugins install/update/remove/list` flows.
//
// Scope (pure logic — no network / subprocess / filesystem side-effects unless
// explicitly stated):
//   * plugin-name sanitisation & path-traversal rejection
//   * Git URL resolution (owner/repo shorthand -> https URL)
//   * repo-name extraction from a git URL
//   * URL scheme classification (secure / insecure / local)
//   * manifest-version compatibility check
//   * `requires_env` entry normalisation (string or {name,...})
//   * disabled-plugins set parsing / serialisation
//   * `requires_hermes` version constraint parsing & comparison
#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <tuple>
#include <unordered_set>
#include <vector>

namespace hermes::cli::plugins_helpers {

inline constexpr int kSupportedManifestVersion = 1;

// ---------------------------------------------------------------------------
// Plugin-name sanitisation.
// ---------------------------------------------------------------------------

// Validate *name* and return the safe target path under *plugins_dir*.
// Throws std::invalid_argument if the name contains path-traversal, is
// empty / ".." / "." or resolves outside *plugins_dir*.
//
// NB: operates on lexically-normalised paths and does NOT touch the real
// filesystem — test-friendly.
std::filesystem::path sanitize_plugin_name(
    const std::string& name,
    const std::filesystem::path& plugins_dir);

// ---------------------------------------------------------------------------
// Git URL resolution.
// ---------------------------------------------------------------------------

enum class UrlScheme {
    Https,
    Http,     // insecure
    Ssh,      // git@ or ssh://
    File,     // local
    Unknown,
};

UrlScheme classify_url(const std::string& url);

// True when the scheme is insecure (`http`) or local (`file`).
bool is_insecure_scheme(const std::string& url);

// Turn an identifier into a cloneable Git URL.
//
// Accepts:
//   * full URLs (https://, http://, git@, ssh://, file://) — returned as-is
//   * "owner/repo" shorthand -> https://github.com/<owner>/<repo>.git
//
// Throws std::invalid_argument for anything else.
std::string resolve_git_url(const std::string& identifier);

// Extract the repo name from a Git URL (for use as the install directory name).
//   * strips trailing "/" and ".git"
//   * returns the last path/colon-separated component
std::string repo_name_from_url(const std::string& url);

// ---------------------------------------------------------------------------
// Manifest handling.
// ---------------------------------------------------------------------------

// True when the manifest version in the YAML dict (integer) is supported.
// A missing key is treated as version 1 (backwards-compat).
bool manifest_version_supported(int manifest_version);

// Describes one entry in `requires_env`.
struct EnvSpec {
    std::string name;
    std::string description;
    std::string url;
    bool secret {false};
};

// Parse a single raw entry.  The first form is the plain string
// ("MY_API_KEY"), the second is the rich dict serialised as a
// `name=... description=... url=... secret=true/false` key-value list.
// Returns std::nullopt if the entry has no `name`.
std::optional<EnvSpec> parse_env_entry_string(const std::string& raw);
std::optional<EnvSpec> parse_env_entry_dict(
    const std::vector<std::pair<std::string, std::string>>& fields);

// ---------------------------------------------------------------------------
// Disabled set (pure file-format I/O).
// ---------------------------------------------------------------------------

// Parse the "disabled-plugins" file (one name per line, empty lines / '#'
// comments ignored).  Duplicates are deduped.
std::unordered_set<std::string> parse_disabled_set(const std::string& file_text);

// Render a disabled set into stable file text (sorted, newline-terminated).
std::string serialise_disabled_set(const std::unordered_set<std::string>& set);

// ---------------------------------------------------------------------------
// Semver compatibility.
// ---------------------------------------------------------------------------

// Parse a dotted numeric version ("1.2.3" / "0.4") into a tuple.
// Returns std::nullopt when the string is empty or any component is
// non-numeric.
std::optional<std::tuple<int, int, int>> parse_semver(const std::string& raw);

// Parse a `>=X.Y[.Z]` / `==X.Y[.Z]` / `>X.Y[.Z]` constraint.
// Returns the operator ("==", ">=", ">", "<=", "<") and the parsed version,
// or nullopt on failure.
std::optional<std::pair<std::string, std::tuple<int, int, int>>>
    parse_constraint(const std::string& raw);

// Test `current` against `constraint`.  Returns false if the constraint
// cannot be parsed.
bool satisfies_constraint(const std::tuple<int, int, int>& current,
                          const std::string& constraint);

// ---------------------------------------------------------------------------
// Plugin-list entry formatting (for `hermes plugins list`).
// ---------------------------------------------------------------------------

// Status of a plugin in the installed list.
enum class PluginStatus {
    Enabled,
    Disabled,
    Broken,  // manifest unreadable / incompatible
};

// Convert to/from a stable string label.
std::string plugin_status_label(PluginStatus s);

// Build the single-line summary shown by `hermes plugins list`.
//   "<name>  [status]  — <description>"
// When description is empty the suffix is omitted.
std::string format_list_line(const std::string& name,
                             PluginStatus status,
                             const std::string& description);

// ---------------------------------------------------------------------------
// Example-file copy helper.
// ---------------------------------------------------------------------------

// Map ".example" filenames to their target basename.
//   "config.yaml.example" -> "config.yaml"
//   "foo.example"         -> "foo"
//   Anything not ending in ".example" -> "".
std::string example_target_name(const std::string& filename);

// ---------------------------------------------------------------------------
// Scheme-warning message.
// ---------------------------------------------------------------------------

// Return the canonical warning string for insecure URL schemes, or empty
// if the URL is secure.
std::string scheme_warning(const std::string& url);

// ---------------------------------------------------------------------------
// After-install banner.
// ---------------------------------------------------------------------------

// Build the default after-install panel text — the message shown when
// the plugin ships no after-install.md.
std::string default_after_install_banner(const std::string& identifier,
                                         const std::string& plugin_dir);

}  // namespace hermes::cli::plugins_helpers
