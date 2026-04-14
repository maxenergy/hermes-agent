// tools_config — C++ port of hermes_cli/tools_config.py.
//
// The Python module drives the interactive `hermes tools` wizard. We
// keep the configuration *data* (toolset labels, defaults, provider
// tables) and the *config-tree mutation* helpers here — enough for
// unit tests and for CLI subcommands like `hermes tools list` /
// `hermes tools enable` / `hermes tools disable`.
//
// Interactive menus are driven by the thin wrapper in tools_config.cpp
// that calls into `curses_ui`.
#pragma once

#include <nlohmann/json.hpp>

#include <ostream>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace hermes::cli::tools_config {

// --- Toolset registry -------------------------------------------------------

struct ToolsetInfo {
    std::string key;          // e.g. "web"
    std::string label;        // "Web Search & Scraping"
    std::string description;  // "web_search, web_extract"
};

// Full list of user-configurable toolsets, in display order.
const std::vector<ToolsetInfo>& configurable_toolsets();

// Look up a toolset by key. Returns nullptr when unknown.
const ToolsetInfo* find_toolset(const std::string& key);

// Toolsets that are OFF by default for new installs.
const std::set<std::string>& default_off_toolsets();

// Effective default selection: everything in configurable_toolsets()
// except the default-off set.
std::set<std::string> default_enabled_toolsets();

// --- Platforms --------------------------------------------------------------

struct PlatformInfo {
    std::string key;                // e.g. "cli"
    std::string label;              // "CLI"
    std::string default_toolset;    // "hermes-cli"
};

const std::vector<PlatformInfo>& platforms();
const PlatformInfo* find_platform(const std::string& key);

// --- Config read / write ---------------------------------------------------

// Read the set of toolsets enabled for a given platform from `config`.
// Falls back to `default_enabled_toolsets()` when the platform has no
// saved config.
std::set<std::string> get_enabled_toolsets(const nlohmann::json& config,
                                           const std::string& platform);

// Write an enabled set back to config under
// `config["platform_toolsets"][platform] = [...]`.
void set_enabled_toolsets(nlohmann::json& config,
                          const std::string& platform,
                          const std::set<std::string>& enabled);

// Enable / disable a single toolset for a single platform. Creates any
// missing config nodes.
void enable_toolset(nlohmann::json& config,
                    const std::string& platform,
                    const std::string& toolset);
void disable_toolset(nlohmann::json& config,
                     const std::string& platform,
                     const std::string& toolset);

// Return the set of platforms that currently have any toolset config
// persisted in `config["platform_toolsets"]`.
std::vector<std::string> configured_platforms(const nlohmann::json& config);

// --- Providers --------------------------------------------------------------

struct ProviderOption {
    std::string name;        // display name
    std::string tag;         // short description / pricing
    std::string backend;     // backend id, e.g. "firecrawl", "edge"
    std::vector<std::string> env_vars;  // env vars the provider requires
    bool requires_nous_auth = false;
};

struct ToolCategory {
    std::string key;                 // toolset key, e.g. "tts"
    std::string name;                 // display name
    std::vector<ProviderOption> providers;
};

// Read the known provider tables (TTS / web / image_gen / browser /
// vision / code_execution / memory / session_search). Only a curated
// subset of the Python map — enough to drive a functional CLI.
const std::vector<ToolCategory>& tool_categories();

const ToolCategory* find_category(const std::string& key);

// Return the environment variable names used by a category. Useful for
// warning the user about missing credentials.
std::vector<std::string> env_vars_for_category(const std::string& key);

// Check whether every env var named in `vars` is set in the process
// environment. Empty / unset values count as missing.
bool env_vars_present(const std::vector<std::string>& vars);

// --- Rendering --------------------------------------------------------------

// Render a one-line status of every toolset for a platform ("[x] web",
// "[ ] vision", etc.). Returns number of lines printed.
std::size_t render_toolset_status(std::ostream& out,
                                  const nlohmann::json& config,
                                  const std::string& platform);

// Render the full `hermes tools list` output (one block per platform).
int cmd_list(std::ostream& out, const nlohmann::json& config);

// --- CLI entry point -------------------------------------------------------

// Top-level dispatcher; mirrors Python `tools_command(args)`.
int dispatch(int argc, char** argv);

}  // namespace hermes::cli::tools_config
