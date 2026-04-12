#include "hermes/tools/send_message_tool.hpp"
#include "hermes/tools/registry.hpp"

#include <stdexcept>
#include <string>

namespace hermes::tools {

ParsedTarget parse_target(std::string_view target) {
    // Expected format: "platform:chat_id:thread_id"
    auto first = target.find(':');
    if (first == std::string_view::npos) {
        throw std::invalid_argument(
            "invalid target format — expected platform:chat_id:thread_id");
    }
    auto second = target.find(':', first + 1);
    if (second == std::string_view::npos) {
        throw std::invalid_argument(
            "invalid target format — expected platform:chat_id:thread_id");
    }

    ParsedTarget pt;
    pt.platform = std::string(target.substr(0, first));
    pt.chat_id = std::string(target.substr(first + 1, second - first - 1));
    pt.thread_id = std::string(target.substr(second + 1));

    if (pt.platform.empty() || pt.chat_id.empty()) {
        throw std::invalid_argument(
            "platform and chat_id must not be empty");
    }
    return pt;
}

namespace {

std::string handle_send_message(const nlohmann::json& args,
                                const ToolContext& /*ctx*/) {
    const auto action = args.at("action").get<std::string>();

    if (action == "list") {
        return tool_error("messaging gateway not initialized");
    }

    if (action != "send") {
        return tool_error("unknown action: " + action);
    }

    const auto target_str = args.at("target").get<std::string>();

    // Validate the target format early — the parsing logic is the
    // valuable part for Phase 8.
    try {
        auto parsed = parse_target(target_str);
        (void)parsed;
    } catch (const std::invalid_argument& ex) {
        return tool_error(ex.what());
    }

    return tool_error("messaging gateway not initialized");
}

}  // namespace

void register_send_message_tools() {
    auto& reg = ToolRegistry::instance();

    ToolEntry e;
    e.name = "send_message";
    e.toolset = "messaging";
    e.description = "Send or list messages via platform gateway";
    e.emoji = "\xF0\x9F\x92\xAC";  // speech bubble
    e.schema = {
        {"type", "object"},
        {"properties",
         {{"action",
           {{"type", "string"},
            {"enum", nlohmann::json::array({"send", "list"})},
            {"description", "send or list"}}},
          {"target",
           {{"type", "string"},
            {"description", "platform:chat_id:thread_id"}}},
          {"content",
           {{"type", "string"}, {"description", "Message body"}}},
          {"file_path",
           {{"type", "string"},
            {"description", "Optional attachment path"}}}}},
        {"required", nlohmann::json::array({"action"})}};
    e.handler = handle_send_message;
    reg.register_tool(std::move(e));
}

}  // namespace hermes::tools
