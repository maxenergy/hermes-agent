// Implementation of ``HermesMcpServer`` (legacy stdio) and ``McpServer``
// (HTTP/SSE). See mcp_server.hpp for the split rationale.
#include "hermes/mcp_server/mcp_server.hpp"

#include <chrono>
#include <iostream>
#include <set>
#include <string>
#include <utility>

namespace hermes::mcp_server {

// ---------------------------------------------------------------------------
// HermesMcpServer (legacy stdio JSON-RPC — unchanged from 0.1 behaviour).
// ---------------------------------------------------------------------------

HermesMcpServer::HermesMcpServer(McpServerConfig config)
    : config_(std::move(config)) {}

void HermesMcpServer::run() {
    while (auto msg = read_message()) {
        auto method = msg->value("method", "");
        auto id = msg->value("id", nlohmann::json());
        auto params = msg->value("params", nlohmann::json::object());

        nlohmann::json response;
        response["jsonrpc"] = "2.0";
        response["id"] = id;

        if (method == "initialize") {
            response["result"] = handle_initialize(params);
        } else if (method == "tools/list") {
            response["result"] = handle_tools_list();
        } else if (method == "tools/call") {
            auto tool_name = params.value("name", "");
            auto args = params.value("arguments", nlohmann::json::object());
            response["result"] = handle_tool_call(tool_name, args);
        } else {
            response["error"] = {
                {"code", -32601}, {"message", "Method not found"}};
        }

        write_message(response);
    }
}

nlohmann::json HermesMcpServer::handle_conversations_list(
    const nlohmann::json& params) {
    int limit = params.value("limit", 50);
    int offset = params.value("offset", 0);
    auto sessions = config_.session_db->list_sessions(limit, offset);

    nlohmann::json result = nlohmann::json::array();
    for (const auto& s : sessions) {
        result.push_back({{"id", s.id},
                          {"source", s.source},
                          {"model", s.model},
                          {"title", s.title.value_or("")}});
    }
    return result;
}

nlohmann::json HermesMcpServer::handle_conversation_get(
    const nlohmann::json& params) {
    auto id = params.value("id", "");
    auto session = config_.session_db->get_session(id);
    if (!session) {
        return {{"error", "not_found"}};
    }
    return {{"id", session->id},
            {"source", session->source},
            {"model", session->model},
            {"title", session->title.value_or("")}};
}

nlohmann::json HermesMcpServer::handle_messages_read(
    const nlohmann::json& params) {
    auto session_id = params.value("session_id", "");
    auto messages = config_.session_db->get_messages(session_id);

    nlohmann::json result = nlohmann::json::array();
    for (const auto& m : messages) {
        result.push_back(
            {{"id", m.id}, {"role", m.role}, {"content", m.content}});
    }
    return result;
}

nlohmann::json HermesMcpServer::handle_messages_send(
    const nlohmann::json& params) {
    auto session_id = params.is_object() ? params.value("session_id", "") : "";
    auto content = params.is_object() ? params.value("content", "") : "";

    if (content.empty()) {
        return {{"error", "content is required"}};
    }

    if (!session_id.empty() && config_.session_db) {
        hermes::state::MessageRow msg;
        msg.session_id = session_id;
        msg.role = "user";
        msg.content = content;
        msg.created_at = std::chrono::system_clock::now();
        config_.session_db->save_message(msg);
    }

    if (config_.agent_factory) {
        try {
            auto response = config_.agent_factory(content);
            if (!session_id.empty() && config_.session_db) {
                hermes::state::MessageRow resp_msg;
                resp_msg.session_id = session_id;
                resp_msg.role = "assistant";
                resp_msg.content = response;
                resp_msg.created_at = std::chrono::system_clock::now();
                config_.session_db->save_message(resp_msg);
            }
            return {{"response", response}};
        } catch (const std::exception& e) {
            return {{"error", e.what()}};
        }
    }

    return {{"response", "agent not configured"},
            {"status", "message_saved"}};
}

nlohmann::json HermesMcpServer::handle_events_poll(
    const nlohmann::json& params) {
    auto session_id = params.is_object() ? params.value("session_id", "") : "";
    int limit = params.is_object() ? params.value("limit", 20) : 20;

    if (session_id.empty() || !config_.session_db) {
        return {{"events", nlohmann::json::array()}};
    }

    auto messages = config_.session_db->get_messages(session_id);

    nlohmann::json events = nlohmann::json::array();
    std::size_t start = 0;
    if (static_cast<int>(messages.size()) > limit) {
        start = messages.size() - static_cast<std::size_t>(limit);
    }
    for (std::size_t i = start; i < messages.size(); ++i) {
        events.push_back({{"id", messages[i].id},
                          {"role", messages[i].role},
                          {"content", messages[i].content},
                          {"session_id", messages[i].session_id}});
    }
    return {{"events", events}};
}

nlohmann::json HermesMcpServer::handle_channels_list(
    const nlohmann::json& params) {
    int limit = params.is_object() ? params.value("limit", 50) : 50;

    if (!config_.session_db) {
        return nlohmann::json::array({nlohmann::json{
            {"name", "default"}, {"type", "conversation"}}});
    }

    auto sessions = config_.session_db->list_sessions(limit, 0);
    std::set<std::string> sources;
    for (const auto& s : sessions) sources.insert(s.source);

    nlohmann::json channels = nlohmann::json::array();
    for (const auto& src : sources) {
        channels.push_back({{"name", src}, {"type", "conversation"}});
    }
    if (channels.empty()) {
        channels.push_back({{"name", "default"}, {"type", "conversation"}});
    }
    return channels;
}

nlohmann::json HermesMcpServer::handle_initialize(
    const nlohmann::json& /*params*/) {
    return {{"protocolVersion", "2024-11-05"},
            {"capabilities",
             {{"tools", nlohmann::json::object()},
              {"resources", nlohmann::json::object()}}},
            {"serverInfo", {{"name", "hermes"}, {"version", "0.1.0"}}}};
}

nlohmann::json HermesMcpServer::handle_tools_list() {
    // clang-format off
    return {{"tools", nlohmann::json::array({
        {{"name", "conversations_list"}, {"description", "List conversations"}, {"inputSchema", {{"type", "object"}, {"properties", {{"limit", {{"type", "integer"}}}, {"offset", {{"type", "integer"}}}}}}}},
        {{"name", "conversation_get"}, {"description", "Get conversation details"}, {"inputSchema", {{"type", "object"}, {"properties", {{"id", {{"type", "string"}}}}}}}},
        {{"name", "messages_read"}, {"description", "Read messages from a conversation"}, {"inputSchema", {{"type", "object"}, {"properties", {{"session_id", {{"type", "string"}}}}}}}},
        {{"name", "messages_send"}, {"description", "Send a message"}, {"inputSchema", {{"type", "object"}, {"properties", {{"session_id", {{"type", "string"}}}, {"content", {{"type", "string"}}}}}}}},
        {{"name", "events_poll"}, {"description", "Poll for new events"}, {"inputSchema", {{"type", "object"}, {"properties", {{"since", {{"type", "string"}}}}}}}},
        {{"name", "channels_list"}, {"description", "List available channels"}, {"inputSchema", {{"type", "object"}, {"properties", nlohmann::json::object()}}}},
        {{"name", "session_search"}, {"description", "Search sessions by text"}, {"inputSchema", {{"type", "object"}, {"properties", {{"query", {{"type", "string"}}}}}}}},
        {{"name", "session_delete"}, {"description", "Delete a session"}, {"inputSchema", {{"type", "object"}, {"properties", {{"id", {{"type", "string"}}}}}}}},
        {{"name", "session_create"}, {"description", "Create a new session"}, {"inputSchema", {{"type", "object"}, {"properties", {{"source", {{"type", "string"}}}, {"model", {{"type", "string"}}}}}}}},
        {{"name", "session_title_update"}, {"description", "Update session title"}, {"inputSchema", {{"type", "object"}, {"properties", {{"id", {{"type", "string"}}}, {"title", {{"type", "string"}}}}}}}}
    })}};
    // clang-format on
}

nlohmann::json HermesMcpServer::handle_tool_call(const std::string& tool_name,
                                                  const nlohmann::json& args) {
    nlohmann::json content;

    if (tool_name == "conversations_list") {
        content = handle_conversations_list(args);
    } else if (tool_name == "conversation_get") {
        content = handle_conversation_get(args);
    } else if (tool_name == "messages_read") {
        content = handle_messages_read(args);
    } else if (tool_name == "messages_send") {
        content = handle_messages_send(args);
    } else if (tool_name == "events_poll") {
        content = handle_events_poll(args);
    } else if (tool_name == "channels_list") {
        content = handle_channels_list(args);
    } else {
        content = {{"error", "unknown_tool"}, {"tool", tool_name}};
    }

    return {{"content",
             nlohmann::json::array(
                 {{{"type", "text"}, {"text", content.dump()}}})}};
}

std::optional<nlohmann::json> HermesMcpServer::read_message() {
    std::string line;
    if (!std::getline(std::cin, line)) return std::nullopt;
    if (line.empty()) return std::nullopt;
    try {
        return nlohmann::json::parse(line);
    } catch (...) {
        return std::nullopt;
    }
}

void HermesMcpServer::write_message(const nlohmann::json& msg) {
    std::cout << msg.dump() << "\n" << std::flush;
}

// ---------------------------------------------------------------------------
// McpServer (HTTP + SSE).
// ---------------------------------------------------------------------------

McpServer::McpServer() : McpServer(Options{}) {}

McpServer::McpServer(Options opts)
    : opts_(std::move(opts)), sessions_(opts_.session_ttl) {
    dispatch_opts_.server_name = opts_.server_name;
    dispatch_opts_.server_version = opts_.server_version;
    dispatch_opts_.instructions = opts_.instructions;
    dispatcher_ = std::make_unique<RpcDispatcher>(dispatch_opts_);
}

McpServer::~McpServer() { stop(); }

void McpServer::set_tool_registry(hermes::tools::ToolRegistry* registry) {
    dispatch_opts_.registry = registry;
    dispatcher_ = std::make_unique<RpcDispatcher>(dispatch_opts_);
}

void McpServer::set_tool_call_hook(ToolCallHook hook) {
    dispatch_opts_.tool_call_hook = std::move(hook);
    dispatcher_ = std::make_unique<RpcDispatcher>(dispatch_opts_);
}

void McpServer::register_resource_provider(
    std::shared_ptr<ResourceProvider> provider) {
    dispatch_opts_.resources = std::move(provider);
    dispatcher_ = std::make_unique<RpcDispatcher>(dispatch_opts_);
}

void McpServer::register_prompt_provider(
    std::shared_ptr<PromptProvider> provider) {
    dispatch_opts_.prompts = std::move(provider);
    dispatcher_ = std::make_unique<RpcDispatcher>(dispatch_opts_);
}

void McpServer::set_session_validator(
    std::function<bool(std::string_view)> validator) {
    session_validator_ = std::move(validator);
    // Currently informational — the dispatcher uses the validator via a
    // wrapper tool_call_hook if the embedder chooses to install one.
    // Exposed so future auth plumbing can query it without re-threading.
}

std::uint16_t McpServer::start() { return start(opts_.port); }

std::uint16_t McpServer::start(std::uint16_t port) {
    if (http_) return http_->listening_port();

    HttpServer::Options hopts;
    hopts.bind_address = opts_.bind_address;
    hopts.port = port;
    hopts.worker_threads = opts_.worker_threads;
    http_ = std::make_unique<HttpServer>(hopts, sessions_, *dispatcher_);
    if (!http_->start()) {
        http_.reset();
        return 0;
    }
    start_gc_thread();
    return http_->listening_port();
}

void McpServer::stop() {
    stop_gc_thread();
    if (http_) {
        http_->stop();
        http_.reset();
    }
    // Shutdown every SSE queue so any leftover clients disconnect.
    for (const auto& id : sessions_.list_ids()) {
        auto s = sessions_.get(id);
        if (s && s->queue) s->queue->shutdown();
    }
}

bool McpServer::running() const { return http_ && http_->running(); }

std::uint16_t McpServer::port() const {
    return http_ ? http_->listening_port() : 0;
}

std::uint64_t McpServer::total_requests() const {
    return http_ ? http_->total_requests() : 0;
}

std::size_t McpServer::session_count() const { return sessions_.size(); }

void McpServer::start_gc_thread() {
    gc_thread_ = std::thread([this] {
        auto interval = opts_.gc_interval;
        std::unique_lock<std::mutex> lk(gc_mu_);
        while (!gc_stop_) {
            // Predicate form → returns true iff predicate holds, so a
            // spurious wake-up + still-unset ``gc_stop_`` keeps us
            // waiting. Important: the predicate is re-evaluated under
            // the lock, closing the tiny race where ``stop_gc_thread``
            // fires ``notify_all`` before we entered wait_for.
            if (gc_cv_.wait_for(lk, interval, [this] { return gc_stop_; })) {
                break;  // gc_stop_ is true
            }
            lk.unlock();
            sessions_.gc_expired();
            lk.lock();
        }
    });
}

void McpServer::stop_gc_thread() {
    {
        std::lock_guard<std::mutex> lk(gc_mu_);
        gc_stop_ = true;
    }
    gc_cv_.notify_all();
    if (gc_thread_.joinable()) gc_thread_.join();
    gc_stop_ = false;  // reset for potential restart (safe: thread joined)
}

}  // namespace hermes::mcp_server
