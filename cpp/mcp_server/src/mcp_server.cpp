#include "hermes/mcp_server/mcp_server.hpp"

#include <iostream>
#include <string>

namespace hermes::mcp_server {

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
    const nlohmann::json& /*params*/) {
    // Stub: message sending requires agent integration
    return {{"status", "not_implemented"}};
}

nlohmann::json HermesMcpServer::handle_events_poll(
    const nlohmann::json& /*params*/) {
    // Stub: event polling requires subscription system
    return {{"events", nlohmann::json::array()}};
}

nlohmann::json HermesMcpServer::handle_channels_list(
    const nlohmann::json& /*params*/) {
    // Stub: returns available communication channels
    return nlohmann::json::array({nlohmann::json{{"name", "default"},
                                                 {"type", "conversation"}}});
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
    if (!std::getline(std::cin, line)) {
        return std::nullopt;
    }
    if (line.empty()) {
        return std::nullopt;
    }
    try {
        return nlohmann::json::parse(line);
    } catch (...) {
        return std::nullopt;
    }
}

void HermesMcpServer::write_message(const nlohmann::json& msg) {
    std::cout << msg.dump() << "\n" << std::flush;
}

}  // namespace hermes::mcp_server
