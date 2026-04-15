// tools_config — C++ port of hermes_cli/tools_config.py.
//
// The Python module drives the interactive `hermes tools` wizard.
// This header exposes both the pure data (toolset registry, providers,
// cost table) and the interactive state-machine hooks used by the
// curses-based editor. Everything is split so unit tests can drive
// state transitions without a TTY.
//
// Surface overview:
//
//   - Toolset / platform metadata      (configurable_toolsets, ...)
//   - Provider/backend metadata        (tool_categories, provider_catalog)
//   - Per-tool allow/deny lists        (AllowDeny, read/apply from config)
//   - Cost preview                      (estimate_monthly_cost)
//   - Config read/write + atomic save  (save_tools_config_atomic)
//   - Interactive editor state-machine (EditorState, apply_key_*)
//   - Top-level CLI dispatcher         (dispatch)
#pragma once

#include <nlohmann/json.hpp>

#include <cstddef>
#include <functional>
#include <optional>
#include <ostream>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace hermes::cli::tools_config {

// ===========================================================================
// Toolset / platform metadata
// ===========================================================================

struct ToolsetInfo {
    std::string key;          // e.g. "web"
    std::string label;        // "Web Search & Scraping"
    std::string description;  // "web_search, web_extract"
    // Approx per-month cost when heavily used (USD). 0 = free / unknown.
    double est_monthly_usd = 0.0;
    // Tools under this toolset (for allow/deny UX).
    std::vector<std::string> tools;
};

const std::vector<ToolsetInfo>& configurable_toolsets();
const ToolsetInfo* find_toolset(const std::string& key);
const std::set<std::string>& default_off_toolsets();
std::set<std::string> default_enabled_toolsets();

struct PlatformInfo {
    std::string key;                // e.g. "cli"
    std::string label;              // "CLI"
    std::string default_toolset;    // "hermes-cli"
    // Env var that, if set, signals the platform is enabled.
    std::string probe_env_var;
};

const std::vector<PlatformInfo>& platforms();
const PlatformInfo* find_platform(const std::string& key);

// Return the set of platforms whose `probe_env_var` is present.
std::vector<std::string> detected_platforms();

// ===========================================================================
// Config read / write
// ===========================================================================

std::set<std::string> get_enabled_toolsets(const nlohmann::json& config,
                                           const std::string& platform);

void set_enabled_toolsets(nlohmann::json& config,
                          const std::string& platform,
                          const std::set<std::string>& enabled);

void enable_toolset(nlohmann::json& config,
                    const std::string& platform,
                    const std::string& toolset);
void disable_toolset(nlohmann::json& config,
                     const std::string& platform,
                     const std::string& toolset);

std::vector<std::string> configured_platforms(const nlohmann::json& config);

// Atomic write of a JSON config object to disk (YAML-wrapped, at the
// hermes home config.yaml path). Returns true on success.
bool save_tools_config_atomic(const nlohmann::json& config);

// Overload: write to a specific path as JSON. Returns true on success.
bool write_json_atomic(const std::string& path, const nlohmann::json& config);

// ===========================================================================
// Per-tool allow/deny lists
// ===========================================================================

struct AllowDeny {
    std::set<std::string> allow;   // if non-empty, only these are allowed
    std::set<std::string> deny;    // these are always blocked
};

// Read `config["tool_acl"][platform]` into an AllowDeny.
AllowDeny read_tool_acl(const nlohmann::json& config,
                        const std::string& platform);

// Write AllowDeny back under `config["tool_acl"][platform]`.
void write_tool_acl(nlohmann::json& config,
                    const std::string& platform,
                    const AllowDeny& acl);

// Return true if `tool` is permitted under the given ACL.
// Semantics:
//   - if deny contains tool           -> false
//   - if allow non-empty and tool not in allow -> false
//   - otherwise                       -> true
bool is_tool_allowed(const AllowDeny& acl, const std::string& tool);

// List of every tool across every toolset. Useful for interactive pickers.
std::vector<std::string> all_tools();

// ===========================================================================
// Providers
// ===========================================================================

struct ProviderOption {
    std::string name;        // display name
    std::string tag;         // short description / pricing
    std::string backend;     // backend id, e.g. "firecrawl", "edge"
    std::vector<std::string> env_vars;  // env vars the provider requires
    bool requires_nous_auth = false;
    // Approx per-call USD cost — cheap heuristic for preview.
    double est_usd_per_call = 0.0;
};

struct ToolCategory {
    std::string key;                 // toolset key, e.g. "tts"
    std::string name;                 // display name
    std::vector<ProviderOption> providers;
};

const std::vector<ToolCategory>& tool_categories();
const ToolCategory* find_category(const std::string& key);
std::vector<std::string> env_vars_for_category(const std::string& key);
bool env_vars_present(const std::vector<std::string>& vars);

// Return the first provider in a category whose env_vars are all set.
// nullptr if none.
const ProviderOption* resolve_active_provider(const ToolCategory& cat);

// Active backend resolved from config or env — e.g. which web backend
// is actually in use. Empty string if none can be determined.
std::string active_backend_for_category(const nlohmann::json& config,
                                        const std::string& category_key);

// ===========================================================================
// Cost preview
// ===========================================================================

struct CostEstimate {
    double monthly_usd = 0.0;
    std::vector<std::pair<std::string, double>> breakdown;  // {toolset, usd}
};

// Estimate monthly cost given enabled toolsets (rough heuristic based
// on est_monthly_usd on ToolsetInfo).
CostEstimate estimate_monthly_cost(const std::set<std::string>& enabled);

// Render a cost estimate to stream.
void render_cost_estimate(std::ostream& out, const CostEstimate& est);

// ===========================================================================
// Rendering (non-interactive)
// ===========================================================================

std::size_t render_toolset_status(std::ostream& out,
                                  const nlohmann::json& config,
                                  const std::string& platform);

int cmd_list(std::ostream& out, const nlohmann::json& config);

// Print a table showing each category + which backend is active.
void render_provider_table(std::ostream& out, const nlohmann::json& config);

// Print the full allow/deny ACL for a platform.
void render_acl(std::ostream& out,
                const nlohmann::json& config,
                const std::string& platform);

// ===========================================================================
// Interactive editor state-machine (testable without a TTY)
// ===========================================================================

enum class EditorView {
    ToolsetList,   // main toggle list
    ProviderList,  // per-category provider selector
    AclEditor,     // allow/deny tool editor
    CostPreview,
};

struct EditorState {
    EditorView view = EditorView::ToolsetList;
    std::string platform = "cli";
    std::size_t cursor = 0;
    std::set<std::string> enabled;
    std::string current_category;   // while in ProviderList/AclEditor
    AllowDeny acl;
    bool dirty = false;
    bool done = false;
    bool cancelled = false;

    // Status line displayed at the bottom of the UI.
    std::string status;
};

// Construct an EditorState seeded from a config + platform.
EditorState make_editor(const nlohmann::json& config,
                        const std::string& platform);

// Apply a single keypress (`key` is an ASCII char + special names
// "UP","DOWN","PGUP","PGDN","ENTER","ESC","SPACE","TAB","q").
// Returns true if the state changed.
bool apply_key(EditorState& state, const std::string& key);

// Flush editor state back into a config JSON (mutates in-place).
void apply_to_config(const EditorState& state, nlohmann::json& config);

// Render the current view to a vector of plain-text lines (one per row).
std::vector<std::string> render_editor(const EditorState& state,
                                       std::size_t width = 80);

// ===========================================================================
// CLI entry point
// ===========================================================================

int dispatch(int argc, char** argv);

// Subcommand handlers, exposed for tests.
int cmd_list_cli(int argc, char** argv);
int cmd_enable_cli(int argc, char** argv);
int cmd_disable_cli(int argc, char** argv);
int cmd_reset_cli(int argc, char** argv);
int cmd_allow_cli(int argc, char** argv);
int cmd_deny_cli(int argc, char** argv);
int cmd_cost_cli(int argc, char** argv);
int cmd_providers_cli(int argc, char** argv);
int cmd_edit_cli(int argc, char** argv);

}  // namespace hermes::cli::tools_config
