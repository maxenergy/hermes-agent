#include "hermes/mcp_server/rpc_dispatch.hpp"

#include "hermes/tools/registry.hpp"

#include <exception>
#include <string>
#include <utility>

namespace hermes::mcp_server {

namespace {

// Convert a ``hermes::llm::ToolSchema`` into the MCP ``tools/list``
// element shape: ``{ name, description, inputSchema }``.
nlohmann::json tool_schema_to_mcp(const hermes::llm::ToolSchema& t) {
    nlohmann::json out;
    out["name"] = t.name;
    out["description"] = t.description;
    if (t.parameters.is_object()) {
        out["inputSchema"] = t.parameters;
    } else {
        out["inputSchema"] = {{"type", "object"}};
    }
    return out;
}

// Wrap an arbitrary JSON payload as an MCP ``tools/call`` response:
// always ``{"content": [{"type":"text","text":"..."}]}``. When the tool
// handler already returns an ``isError`` field, we forward it.
nlohmann::json wrap_tool_result(const nlohmann::json& raw) {
    // If already in MCP content-array shape, pass through untouched.
    if (raw.is_object() && raw.contains("content") && raw["content"].is_array()) {
        return raw;
    }
    nlohmann::json out;
    out["content"] = nlohmann::json::array();
    nlohmann::json text_block;
    text_block["type"] = "text";
    if (raw.is_string()) {
        text_block["text"] = raw.get<std::string>();
    } else {
        text_block["text"] = raw.dump();
    }
    out["content"].push_back(std::move(text_block));
    if (raw.is_object() && raw.contains("error")) out["isError"] = true;
    return out;
}

}  // namespace

RpcDispatcher::RpcDispatcher(Options opts) : opts_(std::move(opts)) {
    if (!opts_.tool_call_hook && opts_.registry) {
        auto* reg = opts_.registry;
        opts_.tool_call_hook = [reg](const std::string& name,
                                     const nlohmann::json& args) {
            hermes::tools::ToolContext ctx;
            ctx.task_id = "mcp";
            ctx.platform = "mcp";
            auto encoded = reg->dispatch(name, args, ctx);
            // ``dispatch`` always returns a JSON string — parse back.
            try {
                return nlohmann::json::parse(encoded);
            } catch (...) {
                nlohmann::json fallback;
                fallback["output"] = encoded;
                return fallback;
            }
        };
    }
}

nlohmann::json RpcDispatcher::handle(
    const RpcRequest& req, const std::shared_ptr<McpSession>& session) {
    if (req.parse_error) {
        return make_error(req.id, rpc_error::kParseError, *req.parse_error);
    }

    try {
        // Notifications have no id; we return a null response and the
        // transport is expected to drop it. ``initialized`` is the one we
        // must actually see; everything else is a no-op.
        if (req.is_notification) {
            if (req.method == "notifications/initialized" ||
                req.method == "initialized") {
                if (session) session->initialized = true;
            }
            return nlohmann::json();  // null → no response
        }

        if (req.method == "initialize") {
            return make_result(req.id, method_initialize(req.params, session));
        }
        if (req.method == "ping") {
            return make_result(req.id, method_ping());
        }
        if (req.method == "tools/list") {
            return make_result(req.id, method_tools_list());
        }
        if (req.method == "tools/call") {
            return make_result(req.id, method_tools_call(req.params));
        }
        if (req.method == "resources/list") {
            return make_result(req.id, method_resources_list());
        }
        if (req.method == "resources/read") {
            return make_result(req.id, method_resources_read(req.params));
        }
        if (req.method == "prompts/list") {
            return make_result(req.id, method_prompts_list());
        }
        if (req.method == "prompts/get") {
            return make_result(req.id, method_prompts_get(req.params));
        }

        return make_error(req.id, rpc_error::kMethodNotFound,
                          "method not found: " + req.method);
    } catch (const std::invalid_argument& ex) {
        return make_error(req.id, rpc_error::kInvalidParams, ex.what());
    } catch (const std::exception& ex) {
        return make_error(req.id, rpc_error::kServerError,
                          std::string("internal error: ") + ex.what());
    } catch (...) {
        return make_error(req.id, rpc_error::kServerError,
                          "internal error (non-std exception)");
    }
}

nlohmann::json RpcDispatcher::handle_raw(
    std::string_view payload, const std::shared_ptr<McpSession>& session) {
    auto req = parse_request(payload);
    return handle(req, session);
}

nlohmann::json RpcDispatcher::method_initialize(
    const nlohmann::json& params, const std::shared_ptr<McpSession>& s) {
    if (s && params.is_object()) {
        auto ci = params.value("clientInfo", nlohmann::json::object());
        if (ci.is_object()) {
            s->client_name = ci.value("name", std::string{});
            s->client_version = ci.value("version", std::string{});
        }
    }

    nlohmann::json capabilities;
    // Advertise the capability blocks matching handlers we actually
    // implement. ``listChanged`` = false since we don't push deltas yet.
    capabilities["tools"] = {{"listChanged", false}};
    if (opts_.resources) {
        capabilities["resources"] = {{"subscribe", false},
                                     {"listChanged", false}};
    }
    if (opts_.prompts) {
        capabilities["prompts"] = {{"listChanged", false}};
    }
    capabilities["logging"] = nlohmann::json::object();

    nlohmann::json out;
    out["protocolVersion"] = std::string(kProtocolVersion);
    out["capabilities"] = std::move(capabilities);
    out["serverInfo"] = {{"name", opts_.server_name},
                         {"version", opts_.server_version}};
    if (!opts_.instructions.empty()) out["instructions"] = opts_.instructions;
    return out;
}

nlohmann::json RpcDispatcher::method_ping() {
    return nlohmann::json::object();  // MCP spec: empty result object.
}

nlohmann::json RpcDispatcher::method_tools_list() {
    nlohmann::json out;
    auto arr = nlohmann::json::array();

    if (opts_.registry) {
        try {
            auto defs = opts_.registry->get_definitions();
            for (const auto& t : defs) {
                arr.push_back(tool_schema_to_mcp(t));
            }
        } catch (const std::exception&) {
            // Leave the array empty on registry failure — don't let a
            // single bad tool schema kill the whole listing.
        }
    }
    out["tools"] = std::move(arr);
    return out;
}

nlohmann::json RpcDispatcher::method_tools_call(const nlohmann::json& params) {
    if (!params.is_object()) {
        throw std::invalid_argument("tools/call params must be an object");
    }
    std::string name = params.value("name", std::string{});
    if (name.empty()) {
        throw std::invalid_argument("tools/call requires \"name\"");
    }
    nlohmann::json args = params.value("arguments", nlohmann::json::object());

    if (!opts_.tool_call_hook) {
        nlohmann::json err;
        err["content"] = nlohmann::json::array();
        err["content"].push_back({{"type", "text"},
                                   {"text", "no tool handler configured"}});
        err["isError"] = true;
        return err;
    }
    try {
        auto raw = opts_.tool_call_hook(name, args);
        return wrap_tool_result(raw);
    } catch (const std::exception& ex) {
        nlohmann::json err;
        err["content"] = nlohmann::json::array();
        err["content"].push_back(
            {{"type", "text"},
             {"text", std::string("tool error: ") + ex.what()}});
        err["isError"] = true;
        return err;
    }
}

nlohmann::json RpcDispatcher::method_resources_list() {
    if (!opts_.resources || !opts_.resources->list) {
        nlohmann::json out;
        out["resources"] = nlohmann::json::array();
        return out;
    }
    auto r = opts_.resources->list();
    if (r.is_array()) {
        nlohmann::json out;
        out["resources"] = std::move(r);
        return out;
    }
    if (r.is_object() && r.contains("resources")) return r;
    nlohmann::json out;
    out["resources"] = nlohmann::json::array();
    return out;
}

nlohmann::json RpcDispatcher::method_resources_read(
    const nlohmann::json& params) {
    if (!opts_.resources || !opts_.resources->read) {
        throw std::invalid_argument("resources/read not supported");
    }
    if (!params.is_object()) {
        throw std::invalid_argument("resources/read params must be an object");
    }
    auto uri = params.value("uri", std::string{});
    if (uri.empty()) {
        throw std::invalid_argument("resources/read requires \"uri\"");
    }
    auto r = opts_.resources->read(uri);
    if (r.is_object() && r.contains("contents")) return r;
    nlohmann::json out;
    out["contents"] = nlohmann::json::array();
    if (!r.is_null()) out["contents"].push_back(r);
    return out;
}

nlohmann::json RpcDispatcher::method_prompts_list() {
    if (!opts_.prompts || !opts_.prompts->list) {
        nlohmann::json out;
        out["prompts"] = nlohmann::json::array();
        return out;
    }
    auto r = opts_.prompts->list();
    if (r.is_array()) {
        nlohmann::json out;
        out["prompts"] = std::move(r);
        return out;
    }
    if (r.is_object() && r.contains("prompts")) return r;
    nlohmann::json out;
    out["prompts"] = nlohmann::json::array();
    return out;
}

nlohmann::json RpcDispatcher::method_prompts_get(const nlohmann::json& params) {
    if (!opts_.prompts || !opts_.prompts->get) {
        throw std::invalid_argument("prompts/get not supported");
    }
    if (!params.is_object()) {
        throw std::invalid_argument("prompts/get params must be an object");
    }
    auto name = params.value("name", std::string{});
    if (name.empty()) {
        throw std::invalid_argument("prompts/get requires \"name\"");
    }
    auto args = params.value("arguments", nlohmann::json::object());
    auto r = opts_.prompts->get(name, args);
    if (r.is_object() && r.contains("messages")) return r;
    nlohmann::json out;
    out["messages"] = nlohmann::json::array();
    return out;
}

}  // namespace hermes::mcp_server
