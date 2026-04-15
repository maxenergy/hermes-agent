// C++17 port of the pure-logic helpers used by
// `hermes_cli/dump.py`'s `run_dump` command.
//
// The full `hermes dump` command reads the environment, talks to
// systemd / launchd, shells out to git, and walks the filesystem.
// Those side effects are out of scope for this port -- we focus on the
// logic that can be unit-tested with deterministic inputs:
//
//   * `redact_secret` -- mirror of the `_redact` helper.
//   * `count_mcp_servers` -- count MCP servers in a config JSON.
//   * `cron_summary` -- parse a `cron/jobs.json` payload into a
//     one-line summary.
//   * `extract_model_and_provider` -- handle dict / string / missing
//     variants of the `model` key.
//   * `detect_memory_provider` -- bounded version of
//     `_memory_provider`.
//   * `detect_configured_platforms` -- return the list of known
//     messaging platforms whose env var is set in a caller-supplied
//     table.
//   * `collect_config_overrides` -- walk the "interesting paths" list
//     and emit `{section.key: value_str}` for non-default values.
//   * `api_keys_table` -- exposes the canonical `(env_var, label)`
//     pairs rendered by `hermes dump` so tests can assert ordering.
//   * `render_dump_api_key_line` -- format one `  <label>  <state>`
//     line, honouring the `show_keys` flag.
//   * `format_version_line` -- build the `version: <ver> (<date>) [<commit>]`
//     line from the three individual fields.
#pragma once

#include <nlohmann/json.hpp>

#include <array>
#include <functional>
#include <map>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace hermes::cli::dump_cmd {

// Redact all but the first 4 and last 4 characters of a secret.  Empty
// strings pass through; strings shorter than 12 characters are replaced
// with a literal `***`.
std::string redact_secret(const std::string& value);

// Count the entries under `config["mcp"]["servers"]`.  Returns 0 when
// any node along the path is missing or not an object.
std::size_t count_mcp_servers(const nlohmann::json& config);

// Parse a cron `jobs.json` payload and render a summary matching the
// Python implementation: `"<active> active / <total> total"` on
// success, `"0"` when the file is absent, or `"(error reading)"` on
// malformed input.  `file_exists` lets the caller inject the "exists"
// signal; `json_body` is the raw file content (may be empty when the
// file is absent).
std::string cron_summary(bool file_exists, const std::string& json_body);

struct model_and_provider {
    std::string model{};
    std::string provider{};
};

// Mirror of `_get_model_and_provider`: accepts a dict with
// `default`/`model`/`name` + `provider` keys, a plain string, or a
// missing node.
model_and_provider extract_model_and_provider(const nlohmann::json& config);

// Return the memory provider name or `"built-in"` when the key is
// missing / empty.
std::string detect_memory_provider(const nlohmann::json& config);

// Mapping from platform name to the env var whose presence signals
// "configured".  The exposed table is a `std::vector<std::pair>` so
// tests can assert ordering.
const std::vector<std::pair<std::string, std::string>>& platform_env_table();

// Given a caller-supplied `env_lookup` callable that returns the raw
// env-var value (empty = unset), return the ordered list of platform
// names whose env var has a non-empty value.
using env_lookup_fn = std::function<std::string(const std::string&)>;

std::vector<std::string> detect_configured_platforms(const env_lookup_fn& env_lookup);

// The `(env_var, label)` pairs rendered under `api_keys:` in the
// dump output, in display order.
const std::vector<std::pair<std::string, std::string>>& api_keys_table();

// Format one API-key line.  `show_keys` toggles between `"set"` /
// `"not set"` and the redacted preview.  The returned string is
// trimmed of trailing whitespace.
std::string render_dump_api_key_line(const std::string& label,
                                     const std::string& value,
                                     bool show_keys);

// Format the single `version: <ver> (<date>) [<commit>]` line given
// the three components.  `release_date` may be empty.
std::string format_version_line(const std::string& version,
                                const std::string& release_date,
                                const std::string& commit);

// The list of "interesting" (section, key) pairs walked by
// `_config_overrides`.
const std::vector<std::pair<std::string, std::string>>& interesting_override_paths();

// Walk the interesting-paths list and return a map of
// `section.key -> str(value)` for non-default user overrides.  The
// defaults JSON object is inspected at the same (section, key)
// coordinates.  Missing sections in either side are treated as empty
// objects.  `toolsets` and `fallback_providers` are handled as
// documented in the Python implementation.  Ordering matches
// `interesting_override_paths()` followed by `toolsets`,
// `fallback_providers`.
std::vector<std::pair<std::string, std::string>> collect_config_overrides(
    const nlohmann::json& config,
    const nlohmann::json& defaults);

}  // namespace hermes::cli::dump_cmd
