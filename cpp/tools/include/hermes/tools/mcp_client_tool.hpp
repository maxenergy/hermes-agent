// MCP client tool — configuration loading and stub tool registration for
// Model Context Protocol servers.  Actual transport is Phase 12+.
#pragma once

#include "hermes/tools/registry.hpp"

#include <map>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include <nlohmann/json.hpp>

namespace hermes::tools {

struct McpServerConfig {
    std::string name;
    std::string command;           // for stdio transport
    std::vector<std::string> args;
    std::string url;               // for HTTP transport
    std::unordered_map<std::string, std::string> headers;
    std::unordered_map<std::string, std::string> env;
    int timeout = 120;
    int connect_timeout = 60;

    struct Sampling {
        bool enabled = false;
        std::string model;
        int max_tokens_cap = 4096;
        int timeout = 30;
        int max_rpm = 10;
    };
    Sampling sampling;
};

class McpClientManager {
public:
    // Parse the "mcpServers" JSON object from config.
    void load_config(const nlohmann::json& mcp_servers_json);

    // List all configured server names.
    std::vector<std::string> server_names() const;

    // Get config for a specific server.
    std::optional<McpServerConfig> get_config(const std::string& name) const;

    // Register stub tools from a server into the given ToolRegistry.
    // Phase 8: registered tools return "MCP server not connected".
    void register_server_tools(const std::string& server_name,
                               ToolRegistry& registry);

    // Connection lifecycle — Phase 12 will implement:
    // void connect(const std::string& server_name);
    // void disconnect(const std::string& server_name);
    // std::string call_tool(const std::string& server,
    //                       const std::string& tool,
    //                       const nlohmann::json& args);

private:
    std::map<std::string, McpServerConfig> configs_;
};

}  // namespace hermes::tools
