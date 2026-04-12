// MCP stdio transport — JSON-RPC 2.0 over child process pipes.
//
// Launches an MCP server as a child process and communicates via its
// stdin/stdout using the newline-delimited JSON-RPC protocol.
#pragma once

#include <chrono>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include <nlohmann/json.hpp>

namespace hermes::tools {

// JSON-RPC 2.0 message used by MCP.
struct JsonRpcMessage {
    std::string jsonrpc = "2.0";
    std::optional<std::string> id;
    std::string method;
    nlohmann::json params;
    nlohmann::json result;
    nlohmann::json error;

    bool is_response() const { return !result.is_null() || !error.is_null(); }
    bool is_request() const { return !method.empty() && !is_response(); }

    // Serialize to JSON object.
    nlohmann::json to_json() const;
    // Parse from JSON object.
    static JsonRpcMessage from_json(const nlohmann::json& j);
};

class McpStdioTransport {
public:
    // Launch the MCP server as a child process (command + args)
    // and communicate via its stdin/stdout.
    McpStdioTransport(const std::string& command,
                      const std::vector<std::string>& args,
                      const std::unordered_map<std::string, std::string>& env = {});
    ~McpStdioTransport();

    // Non-copyable, non-movable (owns file descriptors and child pid).
    McpStdioTransport(const McpStdioTransport&) = delete;
    McpStdioTransport& operator=(const McpStdioTransport&) = delete;

    bool is_connected() const;

    // Send a JSON-RPC request and wait for response (blocking, with timeout).
    nlohmann::json send_request(const std::string& method,
                                const nlohmann::json& params = {},
                                std::chrono::seconds timeout = std::chrono::seconds(120));

    // Send a notification (no response expected).
    void send_notification(const std::string& method,
                           const nlohmann::json& params = {});

    // MCP protocol operations built on top of send_request:
    nlohmann::json initialize(const std::string& client_name = "hermes-cpp",
                              const std::string& client_version = "0.0.1");
    std::vector<nlohmann::json> list_tools();
    nlohmann::json call_tool(const std::string& tool_name,
                             const nlohmann::json& args);

    void shutdown();

    // Build a filtered environment for the child process.  Only safe vars
    // (PATH, HOME, LANG, SHELL, TMPDIR) plus server-specific overrides
    // are included.
    static std::unordered_map<std::string, std::string> build_child_env(
        const std::unordered_map<std::string, std::string>& server_env = {});

    // Exposed for testing.
    void write_message(const nlohmann::json& msg);
    std::optional<nlohmann::json> read_message(std::chrono::seconds timeout);

private:
    int child_pid_ = -1;
    int stdin_fd_ = -1;   // write to child's stdin
    int stdout_fd_ = -1;  // read from child's stdout
    int next_id_ = 1;
    std::mutex mu_;
};

}  // namespace hermes::tools
