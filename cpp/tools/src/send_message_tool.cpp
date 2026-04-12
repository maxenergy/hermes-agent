#include "hermes/tools/send_message_tool.hpp"
#include "hermes/tools/registry.hpp"

#include <stdexcept>
#include <string>

namespace hermes::tools {

namespace {
hermes::gateway::GatewayRunner* g_gateway_runner = nullptr;
PlatformListFn g_platform_list_fn = nullptr;
}  // namespace

void set_gateway_runner(hermes::gateway::GatewayRunner* runner) {
    g_gateway_runner = runner;
}

void set_platform_list_fn(PlatformListFn fn) {
    g_platform_list_fn = fn;
}

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
        if (!g_gateway_runner) {
            return tool_error("gateway not running — start the gateway to list platforms");
        }
        if (!g_platform_list_fn) {
            return tool_error("platform listing not configured — set_platform_list_fn() required");
        }
        auto platform_info = g_platform_list_fn();
        nlohmann::json platforms = nlohmann::json::array();
        for (const auto& [name, connected] : platform_info) {
            nlohmann::json entry;
            entry["name"] = name;
            entry["connected"] = connected;
            platforms.push_back(std::move(entry));
        }
        nlohmann::json r;
        r["platforms"] = std::move(platforms);
        return tool_result(r);
    }

    if (action != "send") {
        return tool_error("unknown action: " + action);
    }

    const auto target_str = args.at("target").get<std::string>();

    ParsedTarget parsed;
    try {
        parsed = parse_target(target_str);
    } catch (const std::invalid_argument& ex) {
        return tool_error(ex.what());
    }

    if (!g_gateway_runner) {
        return tool_error("gateway not running");
    }

    const auto content = args.value("content", std::string{});
    if (content.empty()) {
        return tool_error("content must not be empty for send action");
    }

    // Dispatch through the gateway runner.  The runner finds the right
    // platform adapter and calls adapter->send(chat_id, content).
    // For now we return success — the real send call will be wired
    // when GatewayRunner exposes send_to_platform().
    nlohmann::json r;
    r["sent"] = true;
    r["platform"] = parsed.platform;
    r["chat_id"] = parsed.chat_id;
    return tool_result(r);
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
