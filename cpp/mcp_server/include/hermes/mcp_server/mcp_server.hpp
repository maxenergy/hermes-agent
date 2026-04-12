// HermesMcpServer — exposes Hermes capabilities via the MCP stdio protocol.
#pragma once

#include "hermes/state/session_db.hpp"

#include <optional>
#include <string>

#include <nlohmann/json.hpp>

namespace hermes::mcp_server {

struct McpServerConfig {
    hermes::state::SessionDB* session_db;
};

class HermesMcpServer {
public:
    explicit HermesMcpServer(McpServerConfig config);

    // Start stdio-based MCP server (blocks until stdin closes)
    void run();

    // Individual tool handlers (exposed for testing)
    nlohmann::json handle_conversations_list(const nlohmann::json& params);
    nlohmann::json handle_conversation_get(const nlohmann::json& params);
    nlohmann::json handle_messages_read(const nlohmann::json& params);
    nlohmann::json handle_messages_send(const nlohmann::json& params);
    nlohmann::json handle_events_poll(const nlohmann::json& params);
    nlohmann::json handle_channels_list(const nlohmann::json& params);

    // MCP protocol helpers
    nlohmann::json handle_initialize(const nlohmann::json& params);
    nlohmann::json handle_tools_list();
    nlohmann::json handle_tool_call(const std::string& tool_name,
                                    const nlohmann::json& args);

private:
    McpServerConfig config_;
    // Stdio JSON-RPC line protocol
    std::optional<nlohmann::json> read_message();
    void write_message(const nlohmann::json& msg);
};

}  // namespace hermes::mcp_server
