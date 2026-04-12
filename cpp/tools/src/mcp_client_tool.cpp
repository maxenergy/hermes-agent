#include "hermes/tools/mcp_client_tool.hpp"

#include "hermes/tools/mcp_transport.hpp"
#include "hermes/tools/registry.hpp"

#include <nlohmann/json.hpp>

#include <memory>
#include <string>

namespace hermes::tools {

using json = nlohmann::json;

void McpClientManager::load_config(const json& mcp_servers_json) {
    configs_.clear();
    if (!mcp_servers_json.is_object()) return;

    for (auto& [name, val] : mcp_servers_json.items()) {
        McpServerConfig cfg;
        cfg.name = name;
        cfg.command = val.value("command", "");

        if (val.contains("args") && val["args"].is_array()) {
            for (const auto& a : val["args"]) {
                if (a.is_string()) cfg.args.push_back(a.get<std::string>());
            }
        }

        cfg.url = val.value("url", "");
        cfg.timeout = val.value("timeout", 120);
        cfg.connect_timeout = val.value("connect_timeout", 60);

        if (val.contains("headers") && val["headers"].is_object()) {
            for (auto& [k, v] : val["headers"].items()) {
                if (v.is_string()) cfg.headers[k] = v.get<std::string>();
            }
        }

        if (val.contains("env") && val["env"].is_object()) {
            for (auto& [k, v] : val["env"].items()) {
                if (v.is_string()) cfg.env[k] = v.get<std::string>();
            }
        }

        if (val.contains("sampling") && val["sampling"].is_object()) {
            auto& s = val["sampling"];
            cfg.sampling.enabled = s.value("enabled", false);
            cfg.sampling.model = s.value("model", "");
            cfg.sampling.max_tokens_cap = s.value("max_tokens_cap", 4096);
            cfg.sampling.timeout = s.value("timeout", 30);
            cfg.sampling.max_rpm = s.value("max_rpm", 10);
        }

        configs_[name] = std::move(cfg);
    }
}

std::vector<std::string> McpClientManager::server_names() const {
    std::vector<std::string> names;
    names.reserve(configs_.size());
    for (const auto& [name, _] : configs_) {
        names.push_back(name);
    }
    return names;
}

std::optional<McpServerConfig> McpClientManager::get_config(
    const std::string& name) const {
    auto it = configs_.find(name);
    if (it == configs_.end()) return std::nullopt;
    return it->second;
}

bool McpClientManager::connect(const std::string& server_name) {
    auto it = configs_.find(server_name);
    if (it == configs_.end()) return false;

    const auto& cfg = it->second;
    if (cfg.command.empty()) return false;

    try {
        auto transport = std::make_shared<McpStdioTransport>(
            cfg.command, cfg.args, cfg.env);
        transport->initialize();
        transports_[server_name] = transport;
        return true;
    } catch (...) {
        return false;
    }
}

void McpClientManager::disconnect(const std::string& server_name) {
    auto it = transports_.find(server_name);
    if (it != transports_.end()) {
        it->second->shutdown();
        transports_.erase(it);
    }
}

bool McpClientManager::is_connected(const std::string& server_name) const {
    auto it = transports_.find(server_name);
    if (it == transports_.end()) return false;
    return it->second->is_connected();
}

void McpClientManager::register_server_tools(const std::string& server_name,
                                              ToolRegistry& registry) {
    auto cfg_it = configs_.find(server_name);
    if (cfg_it == configs_.end()) return;

    const auto& cfg = cfg_it->second;

    // If the server has a command, try to connect and discover tools.
    if (!cfg.command.empty()) {
        // Connect if not already connected.
        if (transports_.find(server_name) == transports_.end()) {
            if (!connect(server_name)) {
                // Fall through to stub registration below.
                goto register_stub;
            }
        }

        {
            auto transport = transports_[server_name];
            std::vector<json> tools;
            try {
                tools = transport->list_tools();
            } catch (...) {
                goto register_stub;
            }

            for (const auto& tool_def : tools) {
                ToolEntry e;
                std::string tool_name = tool_def.value("name", "");
                if (tool_name.empty()) continue;

                e.name = "mcp_" + server_name + "_" + tool_name;
                e.toolset = "mcp";
                e.description = tool_def.value("description",
                    "MCP tool: " + tool_name + " (server: " + server_name + ")");
                e.emoji = "\xf0\x9f\x94\x8c";  // plug

                // Use the inputSchema from the tool definition if available.
                if (tool_def.contains("inputSchema")) {
                    e.schema = tool_def["inputSchema"];
                } else {
                    e.schema = json::parse(R"JSON({
                        "type": "object",
                        "properties": {},
                        "additionalProperties": true
                    })JSON");
                }

                // Capture transport and tool_name for the handler.
                auto captured_transport = transport;
                auto captured_tool_name = tool_name;
                auto timeout_secs = cfg.timeout;

                e.handler = [captured_transport, captured_tool_name, timeout_secs](
                                const json& args,
                                const ToolContext& /*ctx*/) -> std::string {
                    try {
                        auto result = captured_transport->call_tool(
                            captured_tool_name, args);
                        return tool_result(result);
                    } catch (const std::exception& ex) {
                        return tool_error(
                            std::string("MCP call failed: ") + ex.what());
                    }
                };

                registry.register_tool(std::move(e));
            }
            return;  // Success — registered real tools.
        }
    }

register_stub:
    // Fallback: register a single stub tool for this MCP server.
    ToolEntry e;
    e.name = "mcp_" + server_name;
    e.toolset = "mcp";
    e.description = "Call MCP server: " + server_name;
    e.emoji = "\xf0\x9f\x94\x8c";  // plug

    e.schema = json::parse(R"JSON({
        "type": "object",
        "properties": {
            "tool": {
                "type": "string",
                "description": "Tool name to call on the MCP server"
            },
            "arguments": {
                "type": "object",
                "description": "Arguments to pass to the tool"
            }
        },
        "required": ["tool"]
    })JSON");

    e.handler = [server_name](const json& /*args*/,
                              const ToolContext& /*ctx*/) -> std::string {
        return tool_error("MCP server not connected: " + server_name);
    };

    registry.register_tool(std::move(e));
}

}  // namespace hermes::tools
