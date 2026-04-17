// Hermes MCP Server — exposes Hermes capabilities to MCP clients.
//
// Two surfaces are provided:
//
//   * ``HermesMcpServer`` — the original stdio JSON-RPC line protocol
//     server. Still used by ``hermes mcp serve`` for editor hosts that
//     spawn the process and communicate over stdin/stdout.
//
//   * ``McpServer`` — the full HTTP + SSE transport implementation with
//     sessioning, resource / prompt providers, and tool registry
//     forwarding. Embedded by the gateway + CLI commands that expose MCP
//     over a network socket.
#pragma once

#include "hermes/mcp_server/http_server.hpp"
#include "hermes/mcp_server/rpc_dispatch.hpp"
#include "hermes/mcp_server/session.hpp"
#include "hermes/state/session_db.hpp"

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

#include <nlohmann/json.hpp>

namespace hermes::tools { class ToolRegistry; }

namespace hermes::mcp_server {

// ---------------------------------------------------------------------------
// Legacy stdio-based server. Kept verbatim for the existing CLI entry point
// + tests; new code should prefer ``McpServer`` below.
// ---------------------------------------------------------------------------
struct McpServerConfig {
    hermes::state::SessionDB* session_db;
    std::function<std::string(const std::string&)> agent_factory;
};

class HermesMcpServer {
public:
    explicit HermesMcpServer(McpServerConfig config);

    void run();

    nlohmann::json handle_conversations_list(const nlohmann::json& params);
    nlohmann::json handle_conversation_get(const nlohmann::json& params);
    nlohmann::json handle_messages_read(const nlohmann::json& params);
    nlohmann::json handle_messages_send(const nlohmann::json& params);
    nlohmann::json handle_events_poll(const nlohmann::json& params);
    nlohmann::json handle_channels_list(const nlohmann::json& params);

    nlohmann::json handle_initialize(const nlohmann::json& params);
    nlohmann::json handle_tools_list();
    nlohmann::json handle_tool_call(const std::string& tool_name,
                                    const nlohmann::json& args);

private:
    McpServerConfig config_;
    std::optional<nlohmann::json> read_message();
    void write_message(const nlohmann::json& msg);
};

// ---------------------------------------------------------------------------
// HTTP + SSE MCP server.
// ---------------------------------------------------------------------------
class McpServer {
public:
    struct Options {
        std::string bind_address = "127.0.0.1";
        std::uint16_t port = 0;  // 0 → OS picks
        std::size_t worker_threads = 2;
        std::chrono::minutes session_ttl{30};
        std::string server_name = "hermes-mcp";
        std::string server_version = "0.1.0";
        std::string instructions;
        std::chrono::seconds gc_interval{60};
    };

    McpServer();
    explicit McpServer(Options opts);
    ~McpServer();

    McpServer(const McpServer&) = delete;
    McpServer& operator=(const McpServer&) = delete;

    // Wire a tool registry for ``tools/list`` + ``tools/call`` forwarding.
    // If never called, the server replies with an empty tool list.
    void set_tool_registry(hermes::tools::ToolRegistry* registry);

    // Override the tool call path entirely — useful for tests.
    void set_tool_call_hook(ToolCallHook hook);

    // Attach resource / prompt providers. Either may be null.
    void register_resource_provider(std::shared_ptr<ResourceProvider> provider);
    void register_prompt_provider(std::shared_ptr<PromptProvider> provider);
    // Attach a completion provider for ``completion/complete``. When null,
    // the server returns an empty completion rather than method_not_found.
    void register_completion_provider(
        std::shared_ptr<CompletionProvider> provider);
    // Override the logging sink invoked on ``logging/setLevel``. Default
    // behaviour forwards to ``hermes::core::logging::setup_logging``.
    void set_logging_sink(LoggingSink sink);

    // Push a server-initiated JSON-RPC notification ( ``id``-less envelope
    // per spec §4.1 ) onto a single session's SSE queue. Returns true when
    // the session exists and the frame was enqueued. ``params`` may be
    // null to emit a bare ``{"jsonrpc":"2.0","method":"..."}``.
    bool send_notification(std::string_view session_id,
                           std::string_view method,
                           const nlohmann::json& params = nlohmann::json());

    // Convenience wrapper: fires ``notifications/resources/updated`` for
    // every session currently subscribed to ``uri``. Returns the number
    // of sessions notified.
    std::size_t notify_resource_updated(std::string_view uri);

    // Optional session validator. When set, any request except
    // ``initialize`` is rejected with a -32000 error unless the validator
    // returns true for the Mcp-Session-Id value.
    void set_session_validator(
        std::function<bool(std::string_view session_id)> validator);

    // Start / stop the HTTP server. ``start(port)`` binds to
    // ``opts_.bind_address`` on the requested port (0 → random). Returns
    // the actually-bound port on success; 0 on failure.
    std::uint16_t start();
    std::uint16_t start(std::uint16_t port);
    void stop();

    bool running() const;
    std::uint16_t port() const;

    // Counters (tests).
    std::uint64_t total_requests() const;
    std::size_t session_count() const;

    // Expose the components so advanced embedders can reach in (e.g. to
    // push server-initiated notifications onto an SSE stream).
    SessionStore& sessions() { return sessions_; }
    RpcDispatcher& dispatcher() { return *dispatcher_; }

private:
    void start_gc_thread();
    void stop_gc_thread();

    Options opts_;
    SessionStore sessions_;
    RpcDispatcher::Options dispatch_opts_;
    std::unique_ptr<RpcDispatcher> dispatcher_;
    std::unique_ptr<HttpServer> http_;
    std::function<bool(std::string_view)> session_validator_;

    std::thread gc_thread_;
    std::mutex gc_mu_;
    std::condition_variable gc_cv_;
    bool gc_stop_ = false;
};

}  // namespace hermes::mcp_server
