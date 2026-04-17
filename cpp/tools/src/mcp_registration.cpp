// MCP tool-registration surface — implementation.
//
// See mcp_registration.hpp for design notes.  All logic here is pure (no
// networking); the MCP client transport is provided by mcp_client_tool
// / mcp_transport.
#include "hermes/tools/mcp_registration.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <regex>
#include <sstream>
#include <string>
#include <unordered_set>

namespace hermes::tools::mcp {

using json = nlohmann::json;

// ---------------------------------------------------------------------------
// Name sanitization
// ---------------------------------------------------------------------------

std::string sanitize_name_component(std::string_view value) {
    std::string out;
    out.reserve(value.size());
    for (char c : value) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '_') {
            out.push_back(c);
        } else {
            out.push_back('_');
        }
    }
    return out;
}

std::string make_prefixed_name(std::string_view server_name,
                               std::string_view tool_name) {
    std::string out = "mcp_";
    out += sanitize_name_component(server_name);
    out += '_';
    out += sanitize_name_component(tool_name);
    return out;
}

// ---------------------------------------------------------------------------
// Schema translation
// ---------------------------------------------------------------------------

json normalize_input_schema(const json& schema) {
    if (schema.is_null()) {
        json j;
        j["type"] = "object";
        j["properties"] = json::object();
        return j;
    }
    if (!schema.is_object()) {
        // Unexpected, but don't crash the agent — coerce into a
        // minimal object schema.
        json j;
        j["type"] = "object";
        j["properties"] = json::object();
        return j;
    }
    // If it claims to be an object but has no properties, add an
    // empty object so downstream serialisers don't emit "properties":
    // null.
    auto type_it = schema.find("type");
    if (type_it != schema.end() && type_it->is_string() &&
        type_it->get<std::string>() == "object" &&
        !schema.contains("properties")) {
        json copy = schema;
        copy["properties"] = json::object();
        return copy;
    }
    return schema;
}

json convert_tool_schema(std::string_view server_name,
                         const json& mcp_tool) {
    std::string raw_name = mcp_tool.value("name", "");
    std::string description = mcp_tool.value("description", "");
    if (description.empty()) {
        std::ostringstream oss;
        oss << "MCP tool " << raw_name << " from " << server_name;
        description = oss.str();
    }
    json schema;
    schema["name"] = make_prefixed_name(server_name, raw_name);
    schema["description"] = description;
    schema["parameters"] = normalize_input_schema(
        mcp_tool.contains("inputSchema") ? mcp_tool["inputSchema"] : json{});
    return schema;
}

// ---------------------------------------------------------------------------
// Filtering
// ---------------------------------------------------------------------------

bool parse_boolish(const json& v, bool default_value) {
    if (v.is_null()) return default_value;
    if (v.is_boolean()) return v.get<bool>();
    if (v.is_number_integer()) return v.get<int>() != 0;
    if (v.is_string()) {
        std::string s = v.get<std::string>();
        // Trim + lower.
        auto a = s.find_first_not_of(" \t\r\n");
        auto b = s.find_last_not_of(" \t\r\n");
        if (a == std::string::npos) return default_value;
        s = s.substr(a, b - a + 1);
        for (auto& c : s) c = static_cast<char>(std::tolower(c));
        if (s == "true" || s == "1" || s == "yes" || s == "on") return true;
        if (s == "false" || s == "0" || s == "no" || s == "off") return false;
    }
    return default_value;
}

std::unordered_set<std::string> normalize_name_filter(const json& v) {
    std::unordered_set<std::string> out;
    if (v.is_null()) return out;
    if (v.is_string()) {
        out.insert(v.get<std::string>());
        return out;
    }
    if (v.is_array()) {
        for (const auto& item : v) {
            if (item.is_string()) out.insert(item.get<std::string>());
        }
    }
    return out;
}

ToolFilter ToolFilter::from_json(const json& tools_block) {
    ToolFilter f;
    if (!tools_block.is_object()) return f;
    if (tools_block.contains("include")) {
        f.include = normalize_name_filter(tools_block["include"]);
    }
    if (tools_block.contains("exclude")) {
        f.exclude = normalize_name_filter(tools_block["exclude"]);
    }
    f.resources_enabled =
        parse_boolish(tools_block.value("resources", json{}), true);
    f.prompts_enabled =
        parse_boolish(tools_block.value("prompts", json{}), true);
    return f;
}

bool ToolFilter::accepts(std::string_view tool_name) const {
    std::string n(tool_name);
    if (!include.empty()) {
        // Whitelist mode — include wins over exclude.
        return include.count(n) > 0;
    }
    return exclude.count(n) == 0;
}

// ---------------------------------------------------------------------------
// Utility schemas
// ---------------------------------------------------------------------------

namespace {

UtilitySchema make_utility(std::string_view server_name,
                           const std::string& suffix,
                           const std::string& description,
                           json parameters,
                           const std::string& handler_key) {
    UtilitySchema u;
    std::string safe = sanitize_name_component(server_name);
    u.schema["name"] = "mcp_" + safe + "_" + suffix;
    u.schema["description"] = description;
    u.schema["parameters"] = std::move(parameters);
    u.handler_key = handler_key;
    return u;
}

}  // namespace

std::vector<UtilitySchema> build_utility_schemas(std::string_view server_name) {
    std::vector<UtilitySchema> out;

    // list_resources
    {
        json params;
        params["type"] = "object";
        params["properties"] = json::object();
        out.push_back(make_utility(
            server_name, "list_resources",
            std::string("List available resources from MCP server '") +
                std::string(server_name) + "'",
            std::move(params), "list_resources"));
    }
    // read_resource
    {
        json params;
        params["type"] = "object";
        json props;
        json uri;
        uri["type"] = "string";
        uri["description"] = "URI of the resource to read";
        props["uri"] = std::move(uri);
        params["properties"] = std::move(props);
        params["required"] = json::array({"uri"});
        out.push_back(make_utility(
            server_name, "read_resource",
            std::string("Read a resource by URI from MCP server '") +
                std::string(server_name) + "'",
            std::move(params), "read_resource"));
    }
    // list_prompts
    {
        json params;
        params["type"] = "object";
        params["properties"] = json::object();
        out.push_back(make_utility(
            server_name, "list_prompts",
            std::string("List available prompts from MCP server '") +
                std::string(server_name) + "'",
            std::move(params), "list_prompts"));
    }
    // get_prompt
    {
        json params;
        params["type"] = "object";
        json props;
        json name;
        name["type"] = "string";
        name["description"] = "Name of the prompt to retrieve";
        props["name"] = std::move(name);
        json args;
        args["type"] = "object";
        args["description"] = "Optional prompt arguments";
        props["arguments"] = std::move(args);
        params["properties"] = std::move(props);
        params["required"] = json::array({"name"});
        out.push_back(make_utility(
            server_name, "get_prompt",
            std::string("Get a prompt by name from MCP server '") +
                std::string(server_name) + "'",
            std::move(params), "get_prompt"));
    }
    return out;
}

std::vector<UtilitySchema> select_utility_schemas(
    std::string_view server_name, const ToolFilter& filter,
    const std::unordered_set<std::string>& advertised_capabilities) {
    std::vector<UtilitySchema> out;
    for (auto& entry : build_utility_schemas(server_name)) {
        const auto& key = entry.handler_key;
        if ((key == "list_resources" || key == "read_resource") &&
            !filter.resources_enabled) {
            continue;
        }
        if ((key == "list_prompts" || key == "get_prompt") &&
            !filter.prompts_enabled) {
            continue;
        }
        if (!advertised_capabilities.count(key)) continue;
        out.push_back(std::move(entry));
    }
    return out;
}

// ---------------------------------------------------------------------------
// Ledger
// ---------------------------------------------------------------------------

void ServerToolLedger::add(const std::string& server,
                           const std::string& tool) {
    by_server_[server].insert(tool);
}

std::vector<std::string> ServerToolLedger::tools_for(
    const std::string& server) const {
    auto it = by_server_.find(server);
    if (it == by_server_.end()) return {};
    std::vector<std::string> out(it->second.begin(), it->second.end());
    std::sort(out.begin(), out.end());
    return out;
}

std::vector<std::string> ServerToolLedger::all_tools() const {
    std::vector<std::string> out;
    for (const auto& [_, set] : by_server_) {
        out.insert(out.end(), set.begin(), set.end());
    }
    std::sort(out.begin(), out.end());
    return out;
}

void ServerToolLedger::clear_server(const std::string& server) {
    by_server_.erase(server);
}

bool ServerToolLedger::has(const std::string& server) const {
    return by_server_.count(server) > 0;
}

std::vector<std::string> ServerToolLedger::servers() const {
    std::vector<std::string> out;
    out.reserve(by_server_.size());
    for (const auto& [k, _] : by_server_) out.push_back(k);
    std::sort(out.begin(), out.end());
    return out;
}

// ---------------------------------------------------------------------------
// Plan / Apply
// ---------------------------------------------------------------------------

RegistrationPlan plan_registration(
    std::string_view server_name,
    const std::vector<json>& mcp_tools, const ToolFilter& filter,
    const std::unordered_set<std::string>& advertised_capabilities) {
    RegistrationPlan plan;
    plan.server_name = std::string(server_name);

    for (const auto& mcp_tool : mcp_tools) {
        std::string raw_name = mcp_tool.value("name", "");
        if (raw_name.empty()) continue;
        if (!filter.accepts(raw_name)) {
            plan.skipped.push_back(raw_name);
            continue;
        }
        plan.tool_schemas.push_back(
            convert_tool_schema(server_name, mcp_tool));
    }
    plan.utility_schemas =
        select_utility_schemas(server_name, filter, advertised_capabilities);
    return plan;
}

std::size_t apply_registration(const RegistrationPlan& plan,
                               ToolRegistry& registry,
                               ServerToolLedger& ledger,
                               const McpHandlerFactory& make_handler,
                               const std::string& toolset_override) {
    std::size_t registered = 0;
    std::string toolset = toolset_override.empty()
                              ? std::string("mcp-") + plan.server_name
                              : toolset_override;

    for (const auto& schema : plan.tool_schemas) {
        ToolEntry entry;
        entry.name = schema.value("name", "");
        if (entry.name.empty()) continue;
        entry.toolset = toolset;
        entry.schema = schema;
        entry.description = schema.value("description", "");
        // Recover the original (unprefixed) tool name from the prefix.
        const std::string prefix =
            "mcp_" + sanitize_name_component(plan.server_name) + "_";
        std::string original = entry.name;
        if (original.rfind(prefix, 0) == 0) {
            original = original.substr(prefix.size());
        }
        if (make_handler) {
            entry.handler = make_handler(plan.server_name, entry.name,
                                         original);
        } else {
            // No factory supplied — the production path (McpClientManager
            // ::register_server_tools) never takes this branch because it
            // binds its own handler directly.  This defensive fallback
            // exists only for callers that build a plan without a
            // connected transport; it reports the actual root cause
            // ("client not connected") rather than the unhelpful legacy
            // "no handler bound" string.  If this branch ever fires in
            // production it is a bug — the parent must bind a real
            // factory via apply_registration's 4th argument.
            const std::string server = plan.server_name;
            const std::string original_tool = original;
            entry.handler = [server, original_tool](const json&,
                                                    const ToolContext&) {
                return tool_error(
                    "MCP client not connected for server '" + server +
                    "' (tool '" + original_tool +
                    "'); apply_registration was called without an "
                    "McpHandlerFactory binding");
            };
        }
        registry.register_tool(std::move(entry));
        ledger.add(plan.server_name, schema.value("name", ""));
        ++registered;
    }

    for (const auto& u : plan.utility_schemas) {
        ToolEntry entry;
        entry.name = u.schema.value("name", "");
        if (entry.name.empty()) continue;
        entry.toolset = toolset;
        entry.schema = u.schema;
        entry.description = u.schema.value("description", "");
        if (make_handler) {
            entry.handler = make_handler(plan.server_name, entry.name,
                                         u.handler_key);
        } else {
            // See explanation above — same defensive path for utility
            // schemas (list_resources / read_resource / ...).
            const std::string server = plan.server_name;
            const std::string handler_key = u.handler_key;
            entry.handler = [server, handler_key](const json&,
                                                  const ToolContext&) {
                return tool_error(
                    "MCP client not connected for server '" + server +
                    "' (utility '" + handler_key +
                    "'); apply_registration was called without an "
                    "McpHandlerFactory binding");
            };
        }
        registry.register_tool(std::move(entry));
        ledger.add(plan.server_name, entry.name);
        ++registered;
    }
    return registered;
}

// ---------------------------------------------------------------------------
// Diff
// ---------------------------------------------------------------------------

ListDiff diff_tool_lists(const std::vector<std::string>& old_names,
                         const std::vector<std::string>& new_names) {
    std::unordered_set<std::string> old_set(old_names.begin(), old_names.end());
    std::unordered_set<std::string> new_set(new_names.begin(), new_names.end());
    ListDiff d;
    for (const auto& n : new_names) {
        if (!old_set.count(n)) d.added.push_back(n);
    }
    for (const auto& n : old_names) {
        if (!new_set.count(n)) d.removed.push_back(n);
    }
    std::sort(d.added.begin(), d.added.end());
    std::sort(d.removed.begin(), d.removed.end());
    return d;
}

// ---------------------------------------------------------------------------
// Env handling
// ---------------------------------------------------------------------------

namespace {

// Keys we always pass through — sufficient for the vast majority of
// stdio MCP servers (npx, python, node binaries, etc.).
bool is_safe_env_key(const std::string& k) {
    static const std::unordered_set<std::string> kAlways = {
        "PATH", "HOME", "USER", "LOGNAME", "LANG", "LC_ALL",
        "TERM", "SHELL", "TMPDIR", "TMP", "TEMP",
        "NODE_PATH", "NODE_OPTIONS", "NPM_CONFIG_PREFIX",
        "PYTHONPATH", "PYTHONHOME", "VIRTUAL_ENV",
        "SSL_CERT_FILE", "SSL_CERT_DIR", "REQUESTS_CA_BUNDLE",
        "HERMES_HOME", "DISPLAY",
    };
    if (kAlways.count(k)) return true;
    // Allow explicit MCP-prefixed keys.
    if (k.rfind("MCP_", 0) == 0) return true;
    // Allow common provider credentials when explicitly set.
    if (k == "GITHUB_PERSONAL_ACCESS_TOKEN" || k == "GITHUB_TOKEN") return true;
    return false;
}

}  // namespace

std::unordered_map<std::string, std::string> build_safe_env(
    const std::unordered_map<std::string, std::string>& env,
    const std::unordered_map<std::string, std::string>& extra) {
    std::unordered_map<std::string, std::string> out;
    for (const auto& [k, v] : env) {
        if (is_safe_env_key(k)) out[k] = v;
    }
    for (const auto& [k, v] : extra) {
        out[k] = v;
    }
    return out;
}

std::string sanitize_error(std::string_view text) {
    std::string out(text);
    // GitHub personal access tokens (fine-grained + classic).
    static const std::regex kPat(
        R"((ghp_|gho_|ghu_|ghs_|ghr_|github_pat_)[A-Za-z0-9_]+)");
    out = std::regex_replace(out, kPat, "[redacted-github-token]");
    // Generic "Bearer <token>" headers.
    static const std::regex kBearer(R"(Bearer\s+[A-Za-z0-9._\-]+)");
    out = std::regex_replace(out, kBearer, "Bearer [redacted]");
    // Generic API key style values (32+ char hex or base64-ish strings
    // following "api_key=" / "key=" / "token=").
    static const std::regex kKeyVal(
        R"((api[_-]?key|token|secret)[=:\s]+[A-Za-z0-9._\-]{16,})",
        std::regex::icase);
    out = std::regex_replace(out, kKeyVal, "$1=[redacted]");
    return out;
}

std::string interpolate_env_vars(std::string_view value) {
    std::string out;
    out.reserve(value.size());
    for (std::size_t i = 0; i < value.size();) {
        if (i + 1 < value.size() && value[i] == '$' && value[i+1] == '{') {
            auto end = value.find('}', i + 2);
            if (end == std::string_view::npos) {
                out.push_back(value[i]);
                ++i;
                continue;
            }
            std::string name(value.substr(i + 2, end - (i + 2)));
            const char* v = std::getenv(name.c_str());
            if (v) {
                out.append(v);
            } else {
                // Leave the literal text alone so configs don't silently
                // pick up empty values.
                out.append(value.substr(i, end - i + 1));
            }
            i = end + 1;
        } else {
            out.push_back(value[i]);
            ++i;
        }
    }
    return out;
}

json interpolate_env_vars_deep(const json& value) {
    if (value.is_string()) {
        return interpolate_env_vars(value.get<std::string>());
    }
    if (value.is_array()) {
        json out = json::array();
        for (const auto& item : value) {
            out.push_back(interpolate_env_vars_deep(item));
        }
        return out;
    }
    if (value.is_object()) {
        json out = json::object();
        for (auto it = value.begin(); it != value.end(); ++it) {
            out[it.key()] = interpolate_env_vars_deep(it.value());
        }
        return out;
    }
    return value;
}

}  // namespace hermes::tools::mcp
