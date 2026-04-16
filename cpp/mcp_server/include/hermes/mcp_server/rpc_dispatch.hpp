// RpcDispatcher — routes a parsed JSON-RPC request to the right MCP handler
// (initialize / tools.* / resources.* / prompts.* / ping) and produces the
// response envelope.
//
// The dispatcher is transport-agnostic: both the SSE + POST /messages path
// and the single-shot POST / path feed decoded payloads here.
#pragma once

#include "hermes/mcp_server/rpc_types.hpp"
#include "hermes/mcp_server/session.hpp"

#include <nlohmann/json.hpp>

#include <functional>
#include <memory>
#include <string>

namespace hermes::tools { class ToolRegistry; }

namespace hermes::mcp_server {

// Provider callbacks. The server never stores MCP resources / prompts in
// memory itself — the embedder (CLI, gateway, tests) supplies the data
// through these lambdas. Both members are optional: when null, the
// corresponding ``resources/*`` or ``prompts/*`` method returns an empty
// list or a ``kMethodNotFound`` error.
struct ResourceProvider {
    // Returns the ``resources`` array for a ``resources/list`` response.
    std::function<nlohmann::json()> list;
    // Reads a resource by uri. Returns ``{"contents": [...]}`` on success
    // or ``{"error": "<msg>"}`` on failure.
    std::function<nlohmann::json(const std::string& uri)> read;
};

struct PromptProvider {
    std::function<nlohmann::json()> list;
    // Returns ``{"messages": [...]}`` on success.
    std::function<nlohmann::json(const std::string& name,
                                  const nlohmann::json& arguments)> get;
};

// Hook called by ``tools/call`` to invoke the underlying tool. Default
// implementation delegates to ``hermes::tools::ToolRegistry`` when one is
// registered; tests can override to inject canned responses.
using ToolCallHook = std::function<nlohmann::json(const std::string& name,
                                                  const nlohmann::json& args)>;

class RpcDispatcher {
public:
    struct Options {
        hermes::tools::ToolRegistry* registry = nullptr;
        std::shared_ptr<ResourceProvider> resources;
        std::shared_ptr<PromptProvider> prompts;
        ToolCallHook tool_call_hook;  // optional override of registry dispatch
        std::string server_name = "hermes-mcp";
        std::string server_version = "0.1.0";
        std::string instructions;
    };

    explicit RpcDispatcher(Options opts);

    // Handle one decoded request. Returns the JSON-RPC response envelope
    // or a null value if the request was a notification (no id).
    nlohmann::json handle(const RpcRequest& req,
                          const std::shared_ptr<McpSession>& session);

    // Run the dispatcher on an arbitrary raw JSON payload. Parse errors
    // surface as a -32700 response.
    nlohmann::json handle_raw(std::string_view payload,
                              const std::shared_ptr<McpSession>& session);

    const Options& options() const { return opts_; }

    // Individual method handlers (public for unit tests).
    nlohmann::json method_initialize(const nlohmann::json& params,
                                     const std::shared_ptr<McpSession>& s);
    nlohmann::json method_ping();
    nlohmann::json method_tools_list();
    nlohmann::json method_tools_call(const nlohmann::json& params);
    nlohmann::json method_resources_list();
    nlohmann::json method_resources_read(const nlohmann::json& params);
    nlohmann::json method_prompts_list();
    nlohmann::json method_prompts_get(const nlohmann::json& params);

private:
    Options opts_;
};

}  // namespace hermes::mcp_server
