// MCP tool-registration surface — ported (partial) from tools/mcp_tool.py.
//
// This header intentionally mirrors the *registration* side of the MCP
// integration (tools/mcp_tool.py lines around `_convert_mcp_schema`,
// `_register_server_tools`, `_sync_mcp_toolsets`, allow/deny filtering,
// and dynamic re-registration on `notifications/tools/list_changed`).
// The RPC client (mcp_client_tool.cpp) is the transport half — this file
// is the schema-translation / filtering / toolset-injection half.
//
// Everything here is testable without a running MCP server: each helper
// is a pure function on the JSON schema payload.
#pragma once

#include "hermes/tools/registry.hpp"

#include <nlohmann/json.hpp>

#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace hermes::tools::mcp {

// ---------------------------------------------------------------------------
// Name sanitization + prefixing
// ---------------------------------------------------------------------------

// Replace every character outside ``[A-Za-z0-9_]`` with ``_``.  Mirrors
// Python ``sanitize_mcp_name_component`` exactly — including the
// observation that hyphens are *not* treated specially (the pattern
// already replaces them with ``_``).
std::string sanitize_name_component(std::string_view value);

// Build the fully-qualified hermes tool name for an MCP tool:
//   mcp_<server>_<tool>
std::string make_prefixed_name(std::string_view server_name,
                               std::string_view tool_name);

// ---------------------------------------------------------------------------
// Schema translation
// ---------------------------------------------------------------------------

// Normalize an MCP ``inputSchema`` blob so it's LLM-tool-calling friendly:
//   * null / missing  -> {"type": "object", "properties": {}}
//   * object without  "properties" -> clone with properties={}
//   * everything else is returned unchanged.
nlohmann::json normalize_input_schema(const nlohmann::json& schema);

// Convert one MCP tool listing entry into the hermes registry schema
// format used by ToolEntry::schema.  ``mcp_tool`` is the JSON object the
// server returned for that tool (keys ``name``, ``description``,
// ``inputSchema``).
nlohmann::json convert_tool_schema(std::string_view server_name,
                                   const nlohmann::json& mcp_tool);

// ---------------------------------------------------------------------------
// Filtering (allow / deny)
// ---------------------------------------------------------------------------

// Container for the per-server filter rules loaded from config.
struct ToolFilter {
    // Exact tool names to include.  When non-empty, *only* these tools
    // (intersected with the server's actual listing) are registered.
    std::unordered_set<std::string> include;
    // Exact tool names to exclude (applied after ``include``).
    std::unordered_set<std::string> exclude;
    // When true, resources/* utility tools (list_resources, ...) are
    // emitted when the server advertises that capability.
    bool resources_enabled = true;
    bool prompts_enabled = true;

    // Parse from the ``tools`` sub-object of an MCP server config.
    static ToolFilter from_json(const nlohmann::json& tools_block);

    // Returns true if ``tool_name`` should be registered.
    bool accepts(std::string_view tool_name) const;
};

// Parse a bool-ish config value (accepts booleans and common strings).
bool parse_boolish(const nlohmann::json& v, bool default_value = true);

// Normalize include/exclude lists (accept string or array of strings).
std::unordered_set<std::string> normalize_name_filter(
    const nlohmann::json& v);

// ---------------------------------------------------------------------------
// Utility tools (resources / prompts) — schema stubs.
// ---------------------------------------------------------------------------

struct UtilitySchema {
    nlohmann::json schema;
    std::string handler_key;  // "list_resources" | "read_resource" | ...
};

// The four utility schemas for a given server.  Ordering is stable.
std::vector<UtilitySchema> build_utility_schemas(std::string_view server_name);

// Filter the utility schemas using a ToolFilter and the server's
// advertised capabilities.  ``advertised_capabilities`` is a set of
// strings like {"list_resources","read_resource","list_prompts",
// "get_prompt"}.
std::vector<UtilitySchema> select_utility_schemas(
    std::string_view server_name,
    const ToolFilter& filter,
    const std::unordered_set<std::string>& advertised_capabilities);

// ---------------------------------------------------------------------------
// Per-server registration state
// ---------------------------------------------------------------------------

// Tracks which registry names are owned by an MCP server so a
// `list_changed` notification can cleanly refresh them.
class ServerToolLedger {
public:
    // Set of registered tool names for a given server.
    void add(const std::string& server, const std::string& tool);
    std::vector<std::string> tools_for(const std::string& server) const;
    std::vector<std::string> all_tools() const;
    void clear_server(const std::string& server);
    bool has(const std::string& server) const;

    std::vector<std::string> servers() const;

private:
    std::unordered_map<std::string, std::unordered_set<std::string>> by_server_;
};

// ---------------------------------------------------------------------------
// Registration planner
// ---------------------------------------------------------------------------

// A fully-resolved registration plan for one MCP server.  Produced by
// plan_registration() and consumed by apply_registration() — splitting
// these lets tests exercise the plan synthesis without needing a live
// ToolRegistry.
struct RegistrationPlan {
    std::string server_name;
    // Renamed tools from the server listing (already prefixed and
    // schema-translated).
    std::vector<nlohmann::json> tool_schemas;
    // Utility tool schemas (resources/prompts) selected by capability.
    std::vector<UtilitySchema> utility_schemas;
    // Names the plan skipped because of filter rules (recorded for
    // logging / audit).
    std::vector<std::string> skipped;
};

// Build the registration plan for a server, given its tool listing, the
// per-server filter, and the set of advertised capabilities.
RegistrationPlan plan_registration(
    std::string_view server_name,
    const std::vector<nlohmann::json>& mcp_tools,
    const ToolFilter& filter,
    const std::unordered_set<std::string>& advertised_capabilities);

// Apply a plan to a live ToolRegistry.  Handlers are created by
// ``make_handler`` — the translation unit owning it typically binds it
// to the MCP client RPC path.  The ledger is updated in place; callers
// should clear_server(name) before calling this to replace tools.
//
// Returns the number of tools registered.
using McpHandlerFactory =
    std::function<ToolHandler(const std::string& server_name,
                              const std::string& prefixed_name,
                              const std::string& original_name)>;
std::size_t apply_registration(const RegistrationPlan& plan,
                               ToolRegistry& registry,
                               ServerToolLedger& ledger,
                               const McpHandlerFactory& make_handler,
                               const std::string& toolset_override = "");

// ---------------------------------------------------------------------------
// Dynamic refresh
// ---------------------------------------------------------------------------

// Compute the diff (added/removed tool names) between an old listing and
// a new one.  Used to log what changed when a server sends
// ``notifications/tools/list_changed``.
struct ListDiff {
    std::vector<std::string> added;
    std::vector<std::string> removed;
};
ListDiff diff_tool_lists(const std::vector<std::string>& old_names,
                         const std::vector<std::string>& new_names);

// ---------------------------------------------------------------------------
// Env filtering for stdio launches.
// ---------------------------------------------------------------------------

// Return a copy of ``env`` with only the keys deemed safe to pass to a
// stdio MCP child process (mirrors Python ``_build_safe_env``).  When
// ``extra`` is supplied its entries override/augment the filtered base.
std::unordered_map<std::string, std::string> build_safe_env(
    const std::unordered_map<std::string, std::string>& env,
    const std::unordered_map<std::string, std::string>& extra = {});

// Sanitize an error message for LLM consumption (strips common PAT/token
// patterns).
std::string sanitize_error(std::string_view text);

// Interpolate ``${ENV_VAR}`` placeholders inside a string against the
// caller's environment.  Missing variables are left intact.
std::string interpolate_env_vars(std::string_view value);

// Recursive interpolation — applies interpolate_env_vars to every
// string leaf of a JSON value.
nlohmann::json interpolate_env_vars_deep(const nlohmann::json& value);

}  // namespace hermes::tools::mcp
