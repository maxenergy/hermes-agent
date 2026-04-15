// C++17 port of the pure-logic helpers from
// `hermes_cli/memory_setup.py`.
//
// `hermes memory setup` is driven by a curses UI, plugin discovery via
// `plugins.memory`, and pip/uv subprocess invocations for dependency
// installation.  Those concerns require runtime singletons that the
// C++ port doesn't own yet; instead this header exposes the pure-logic
// building blocks the command depends on so the rest of the port can
// be wired together later:
//
//   * `classify_setup_hint` -- derive the "local" / "requires API key"
//     / "API key / local" / "no setup needed" hint shown beside each
//     provider in the picker.
//   * `pip_to_import_name` -- translate a pip package name to its
//     importable module name, matching the `_IMPORT_NAMES` table plus
//     the "replace dashes with underscores" fallback.
//   * `compute_missing_pip_dependencies` -- given the pip deps listed
//     in a plugin's `plugin.yaml` and a callable returning "installed?"
//     per import name, return the list of missing packages in
//     declaration order.
//   * `render_env_file_update` -- the `.env` file merge used by
//     `_write_env_vars`.  Preserves existing lines, replaces in place
//     when a key already exists, and appends new keys at the end.
//   * `mask_existing_secret` -- the `"...1234"` / `"set"` preview
//     string shown when re-prompting for an API key.
//   * `render_provider_status_lines` -- mirrors the plain-text output
//     of `cmd_status` for a given provider set.  The caller supplies
//     the provider list (from plugin discovery) and the active
//     provider name; the helper returns the rendered lines in order.
//   * `format_provider_config_line` -- the `    key: value` line
//     written under `  <provider> config:`.
#pragma once

#include <nlohmann/json.hpp>

#include <functional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace hermes::cli::memory_setup {

// Setup-hint classification outcome.  Mirrors the Python string
// constants so tests can assert exact wording.
enum class setup_hint {
    requires_api_key,   // "requires API key"
    api_key_or_local,   // "API key / local"
    no_setup_needed,    // "no setup needed"
    local,              // "local"
};

std::string setup_hint_label(setup_hint hint);

// Config schema entry -- only the subset we inspect (the `secret`
// flag).  Extra keys from YAML are ignored.
struct schema_field {
    bool secret{false};
    std::string key{};
    std::string env_var{};
};

// Classify a provider's setup hint from its config schema.
setup_hint classify_setup_hint(const std::vector<schema_field>& schema);

// Translate a pip package name into the import name, matching the
// hard-coded `_IMPORT_NAMES` table and the `replace("-", "_").split("[")[0]`
// fallback used by the Python implementation.
std::string pip_to_import_name(const std::string& pip_name);

// Return the pip dependencies that are not importable.  `is_installed`
// is invoked with the import name (as returned by
// `pip_to_import_name`); return true if the module can be imported.
std::vector<std::string> compute_missing_pip_dependencies(
    const std::vector<std::string>& pip_deps,
    const std::function<bool(const std::string&)>& is_installed);

// Merge `updates` into the existing `.env` file content.  Lines whose
// `KEY=...` matches an update are replaced in place; the remaining
// updates are appended at the end.  Trailing newline matches the
// Python implementation (`"\n".join(...) + "\n"`).
std::string render_env_file_update(
    const std::string& existing_content,
    const std::vector<std::pair<std::string, std::string>>& updates);

// Render the `"...1234"` preview used when prompting for a secret
// whose current value is still set.  Values shorter than 5 characters
// fall back to the literal `"set"`.
std::string mask_existing_secret(const std::string& value);

// One entry in the discovered-providers list used by
// `render_provider_status_lines`.
struct discovered_provider {
    std::string name{};
    std::string description{};  // setup hint label
    bool available{false};
    // Secret fields expected by the provider -- used to render the
    // "Missing:" list when the provider is not available.  Empty when
    // the provider is available or needs no secrets.
    std::vector<schema_field> secret_fields{};
};

// Status-command rendering context.
struct status_context {
    std::string active_provider{};
    // Per-provider key/value pairs recorded under
    // `config["memory"][<provider>]`.  Rendered in insertion order.
    std::vector<std::pair<std::string, std::string>> active_provider_config{};
    // Full list of providers discovered from the plugin directory.
    std::vector<discovered_provider> providers{};
    // Callback returning true if the env var is set.  Used for the
    // "Missing:" list markers.  If null, all env vars are treated as
    // unset.
    std::function<bool(const std::string&)> env_is_set{};
};

std::vector<std::string> render_provider_status_lines(const status_context& ctx);

// Format one `    <key>: <value>` config row.
std::string format_provider_config_line(const std::string& key,
                                        const std::string& value);

// Known pip-to-import mappings exposed for tests.
const std::unordered_map<std::string, std::string>& pip_import_overrides();

}  // namespace hermes::cli::memory_setup
