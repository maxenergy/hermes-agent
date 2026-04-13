// MCP stdio transport — JSON-RPC 2.0 over child process pipes.
//
// Launches an MCP server as a child process and communicates via its
// stdin/stdout using the newline-delimited JSON-RPC protocol.
#pragma once

#include <chrono>
#include <functional>
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

    // Server-to-client request/notification handler.  When a message arrives
    // from the server whose id doesn't match a pending request (or which has
    // no id — i.e. a notification), it is dispatched to this callback from
    // within send_request's wait loop.  Handlers for *requests* must return
    // a JSON value used as the ``result`` of the response.  For
    // *notifications* (no id), the return value is ignored.
    //
    // If the handler throws, a JSON-RPC error with code -32603 is returned.
    // If no handler is installed, inbound server requests get a method-not-
    // found error (-32601) and notifications are silently dropped.
    //
    // The method and params are passed to the callback.
    using InboundHandler = std::function<nlohmann::json(
        const std::string& method, const nlohmann::json& params)>;
    void set_inbound_handler(InboundHandler handler);

    // Drain any pending messages (notifications / requests) from the
    // server without blocking on a matching response id.  Useful for
    // catching ``notifications/tools/list_changed`` outside of a call.
    // Returns the number of messages processed.
    int pump_messages(std::chrono::milliseconds budget =
                          std::chrono::milliseconds(10));

    // Build a filtered environment for the child process.  Only safe vars
    // (PATH, HOME, LANG, SHELL, TMPDIR) plus server-specific overrides
    // are included.
    static std::unordered_map<std::string, std::string> build_child_env(
        const std::unordered_map<std::string, std::string>& server_env = {});

    // Exposed for testing.
    void write_message(const nlohmann::json& msg);
    std::optional<nlohmann::json> read_message(std::chrono::seconds timeout);

private:
    // Dispatch an inbound server-originated message.  If it's a request
    // (has a method + id), invokes the installed handler and writes back
    // a response.  If it's a notification (method but no id), invokes the
    // handler and ignores the return value.  Returns true if the message
    // was consumed (i.e. it was not a response we should forward to a
    // caller of send_request).
    bool handle_inbound_(const nlohmann::json& msg);

    int child_pid_ = -1;
    int stdin_fd_ = -1;   // write to child's stdin
    int stdout_fd_ = -1;  // read from child's stdout
    int next_id_ = 1;
    std::mutex mu_;
    InboundHandler inbound_handler_;
    // Pending partial read buffer (for read_message across fragments).
    std::string read_buf_;
};

}  // namespace hermes::tools
