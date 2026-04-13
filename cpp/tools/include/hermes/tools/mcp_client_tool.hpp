// MCP client tool — configuration loading and tool registration for
// Model Context Protocol servers via stdio transport.
#pragma once

#include "hermes/tools/mcp_transport.hpp"
#include "hermes/tools/registry.hpp"

#include <chrono>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include <nlohmann/json.hpp>

namespace hermes::llm {
class LlmClient;
}

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

    // Reconnect settings.  If ``reconnect_enabled`` is true, McpClientManager
    // will attempt to re-launch the stdio child on transport failure with
    // exponential backoff (base ``reconnect_initial_ms``, multiplier 2,
    // capped at ``reconnect_max_ms``).  A small jitter (±20%) is added to
    // each delay, matching the server-side ``jittered_backoff`` contract.
    bool reconnect_enabled = true;
    int reconnect_initial_ms = 500;
    int reconnect_max_ms = 30000;
    int reconnect_max_attempts = 5;
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

    // -- Sampling / dynamic discovery / OAuth wiring ------------------

    // If set, incoming ``sampling/createMessage`` server requests are routed
    // to this LLM client.  When null, sampling returns a JSON-RPC error.
    void set_llm_client(hermes::llm::LlmClient* client);

    // Approval gate for sampling.  Defaults to false — sampling requests
    // are rejected unless explicitly allowed.  This maps to the Python
    // ``config.get("mcp.allow_sampling", False)`` flag.
    void set_allow_sampling(bool allow);
    bool allow_sampling() const;

    // Optional user-approval hook (called before sampling is executed).
    // Return true to approve, false to reject.  When null and
    // ``allow_sampling`` is true, sampling proceeds without prompting.
    using SamplingApprover =
        std::function<bool(const std::string& server_name,
                           const nlohmann::json& params)>;
    void set_sampling_approver(SamplingApprover approver);

    // Attempt to (re)discover tools for a server and refresh the registry
    // entries previously registered as MCP-owned (toolset == "mcp" and
    // name starts with "mcp_<server>_").  Invoked automatically when the
    // server sends ``notifications/tools/list_changed``.
    void refresh_server_tools(const std::string& server_name,
                              ToolRegistry& registry);

    // Called by the transport's inbound handler for server→client messages.
    // Exposed for tests; normal code uses the transport-installed handler.
    nlohmann::json handle_inbound_(const std::string& server_name,
                                   const std::string& method,
                                   const nlohmann::json& params);

    // -- OAuth wiring -------------------------------------------------

    // Install a callback that runs an OAuth flow for ``server_name`` and
    // returns the access token on success (or empty string to abort).
    // Invoked by connect() when the configured connect() raises and the
    // cause looks like a 401 / insufficient auth.  (For HTTP MCP transport
    // this is invoked when the server returns ``WWW-Authenticate: Bearer``;
    // stdio transport passes the token through the ``env`` map as
    // ``MCP_ACCESS_TOKEN`` before relaunching.)
    using OAuthInitiator =
        std::function<std::string(const std::string& server_name,
                                   const std::string& www_authenticate)>;
    void set_oauth_initiator(OAuthInitiator fn);

    // Force-run the OAuth flow for ``server_name`` (no 401 needed).
    // Returns the resulting access token (empty on failure).
    std::string run_oauth_flow(const std::string& server_name,
                               const std::string& www_authenticate = "");

    // Test hook: inject a transport for a given server name.
    void inject_transport_for_testing(
        const std::string& server_name,
        std::shared_ptr<McpStdioTransport> transport);

private:
    // Internal: connect with reconnect/backoff.  Returns true on success.
    bool connect_with_backoff_(const std::string& server_name);

    // Install the transport-level inbound handler that routes incoming
    // method calls through handle_inbound_().
    void install_inbound_handler_(const std::string& server_name,
                                  ToolRegistry* registry);

    std::map<std::string, McpServerConfig> configs_;
    std::map<std::string, std::shared_ptr<McpStdioTransport>> transports_;
    // Names of registry entries we registered per server — cleaned up and
    // re-registered when the tool list changes.
    std::map<std::string, std::set<std::string>> server_tool_names_;
    // Registry pointer captured at register_server_tools() time so
    // dynamic-discovery events can re-register without a parameter.
    std::map<std::string, ToolRegistry*> server_registry_;

    hermes::llm::LlmClient* llm_client_ = nullptr;
    bool allow_sampling_ = false;
    SamplingApprover sampling_approver_;
    OAuthInitiator oauth_initiator_;

    // Track OAuth tokens per server (in-memory cache in addition to
    // McpOAuth::load_token disk cache).
    std::map<std::string, std::string> oauth_tokens_;
    mutable std::mutex mu_;
};

}  // namespace hermes::tools
