// MCP client tool — configuration loading and tool registration for
// Model Context Protocol servers via stdio transport.
#pragma once

#include "hermes/tools/mcp_transport.hpp"
#include "hermes/tools/registry.hpp"

#include <map>
#include <memory>
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

    // Connect to an MCP server via stdio transport (launches child process).
    // Returns true on success.
    bool connect(const std::string& server_name);

    // Disconnect a running MCP server.
    void disconnect(const std::string& server_name);

    // Check if a server is connected.
    bool is_connected(const std::string& server_name) const;

    // Register tools discovered from an MCP server into the given ToolRegistry.
    // If the server has a `command` configured, connects via stdio transport,
    // discovers tools via tools/list, and registers real handlers.
    // Falls back to a proxy tool if connection fails or no command is configured.
    void register_server_tools(const std::string& server_name,
                               ToolRegistry& registry);

private:
    std::map<std::string, McpServerConfig> configs_;
    std::map<std::string, std::shared_ptr<McpStdioTransport>> transports_;
};

}  // namespace hermes::tools
