// C++17 port of hermes_cli/mcp_config.py — `hermes mcp` subcommand.
//
// Covers add/remove/list/test/configure/enable/disable of MCP server
// entries in ~/.hermes/config.yaml.  The C++ port stores the same shape
// — `mcp_servers: { <name>: { url|command, args, headers, auth,
// enabled, tools: { include|exclude } } }` — and shares the same
// env-var interpolation + auth-masking logic as the Python original.
//
// Because the full MCP SDK handshake is not yet available in C++, the
// `test` subcommand performs a best-effort HTTP probe (HEAD/GET) for
// `url`-type servers and reports stdio servers by inspecting whether
// the command is executable on `$PATH`.  Interactive tool-selection
// (curses_checklist in Python) degrades gracefully to an "enable all"
// default when the caller is not a TTY.
#pragma once

#include <nlohmann/json.hpp>

#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace hermes::cli::mcp_config {

// -----------------------------------------------------------------
// In-memory representation of one MCP server entry.
// -----------------------------------------------------------------
struct ServerConfig {
    std::string name;
    // Transport:  either `url` (HTTP) or `command` (stdio).
    std::string url;
    std::string command;
    std::vector<std::string> args;

    // Auth (either explicit auth_type string, or a headers map).
    std::string auth_type;  // "oauth", "header", "" (none)
    std::map<std::string, std::string> headers;

    // Tool filter.
    std::vector<std::string> include;
    std::vector<std::string> exclude;

    // Gate flag.
    bool enabled = true;

    // Round-tripped freeform options that we don't introspect.
    nlohmann::json extra = nlohmann::json::object();

    bool is_http() const { return !url.empty(); }
    bool is_stdio() const { return url.empty() && !command.empty(); }

    nlohmann::json to_json() const;
    static ServerConfig from_json(const std::string& name,
                                  const nlohmann::json& j);
};

// Compute the canonical env-var key for a server name:
//   `MCP_<UPPER>_API_KEY`, hyphens mapped to underscores.
std::string env_key_for_server(const std::string& server_name);

// Expand `${VAR}` placeholders against the process environment.
std::string interpolate_env(const std::string& value);

// Mask an authorization value for display: `abcd…wxyz` or `***`.
std::string mask_auth_value(const std::string& value);

// -----------------------------------------------------------------
// Config file I/O — backed by hermes::config::{load,save}_config.
// -----------------------------------------------------------------
std::map<std::string, ServerConfig> list_servers();
std::optional<ServerConfig> get_server(const std::string& name);
bool save_server(const ServerConfig& server);
bool remove_server(const std::string& name);

// -----------------------------------------------------------------
// Rendering helpers (unit-testable — pure formatting).
// -----------------------------------------------------------------
struct RenderedRow {
    std::string name;
    std::string transport;
    std::string tools_str;
    std::string status;
};
RenderedRow render_row(const ServerConfig& server);
std::vector<std::string> render_table(
    const std::map<std::string, ServerConfig>& servers);

// -----------------------------------------------------------------
// Handshake probe (HTTP-only best effort).  Returns a short
// descriptor of the reachability check result.
// -----------------------------------------------------------------
struct ProbeResult {
    bool ok = false;
    std::string message;
    int elapsed_ms = 0;
};
ProbeResult probe_server(const ServerConfig& server);

// -----------------------------------------------------------------
// CLI entry point.
// -----------------------------------------------------------------
int run(int argc, char* argv[]);

// Subcommand handlers — broken out for tests.
int cmd_list(const std::vector<std::string>& argv);
int cmd_add(const std::vector<std::string>& argv);
int cmd_remove(const std::vector<std::string>& argv);
int cmd_test(const std::vector<std::string>& argv);
int cmd_enable(const std::vector<std::string>& argv, bool enable);
int cmd_configure(const std::vector<std::string>& argv);

}  // namespace hermes::cli::mcp_config
