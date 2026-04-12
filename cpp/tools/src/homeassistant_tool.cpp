#include "hermes/tools/homeassistant_tool.hpp"

#include "hermes/tools/registry.hpp"

#include <nlohmann/json.hpp>

#include <cstdlib>
#include <string>

namespace hermes::tools {

namespace {

bool ha_env_available() {
    return std::getenv("HA_URL") != nullptr &&
           std::getenv("HA_TOKEN") != nullptr;
}

std::string ha_url() {
    const char* v = std::getenv("HA_URL");
    return v ? std::string(v) : std::string();
}

std::string ha_token() {
    const char* v = std::getenv("HA_TOKEN");
    return v ? std::string(v) : std::string();
}

}  // namespace

void register_homeassistant_tools(ToolRegistry& registry) {
    // Shared toolset check
    registry.register_toolset_check("homeassistant", ha_env_available);

    // ----- ha_list_entities -----
    {
        ToolEntry e;
        e.name = "ha_list_entities";
        e.toolset = "homeassistant";
        e.description = "List all Home Assistant entities";
        e.emoji = "\xf0\x9f\x8f\xa0";  // house
        e.check_fn = ha_env_available;
        e.schema = nlohmann::json::parse(R"JSON({
            "type": "object",
            "properties": {}
        })JSON");

        e.handler = [](const nlohmann::json& /*args*/,
                       const ToolContext& /*ctx*/) -> std::string {
            // TODO(phase-9): wire cpr
            auto url = ha_url() + "/api/states";
            (void)url;
            (void)ha_token();
            return tool_error("HTTP transport not available");
        };

        registry.register_tool(std::move(e));
    }

    // ----- ha_get_state -----
    {
        ToolEntry e;
        e.name = "ha_get_state";
        e.toolset = "homeassistant";
        e.description = "Get the state of a Home Assistant entity";
        e.emoji = "\xf0\x9f\x8f\xa0";
        e.check_fn = ha_env_available;
        e.schema = nlohmann::json::parse(R"JSON({
            "type": "object",
            "properties": {
                "entity_id": {
                    "type": "string",
                    "description": "The entity ID (e.g. light.living_room)"
                }
            },
            "required": ["entity_id"]
        })JSON");

        e.handler = [](const nlohmann::json& args,
                       const ToolContext& /*ctx*/) -> std::string {
            if (!args.contains("entity_id") ||
                !args["entity_id"].is_string()) {
                return tool_error("missing required parameter: entity_id");
            }
            // TODO(phase-9): wire cpr
            auto url = ha_url() + "/api/states/" +
                       args["entity_id"].get<std::string>();
            (void)url;
            (void)ha_token();
            return tool_error("HTTP transport not available");
        };

        registry.register_tool(std::move(e));
    }

    // ----- ha_list_services -----
    {
        ToolEntry e;
        e.name = "ha_list_services";
        e.toolset = "homeassistant";
        e.description = "List available Home Assistant services";
        e.emoji = "\xf0\x9f\x8f\xa0";
        e.check_fn = ha_env_available;
        e.schema = nlohmann::json::parse(R"JSON({
            "type": "object",
            "properties": {}
        })JSON");

        e.handler = [](const nlohmann::json& /*args*/,
                       const ToolContext& /*ctx*/) -> std::string {
            // TODO(phase-9): wire cpr
            auto url = ha_url() + "/api/services";
            (void)url;
            (void)ha_token();
            return tool_error("HTTP transport not available");
        };

        registry.register_tool(std::move(e));
    }

    // ----- ha_call_service -----
    {
        ToolEntry e;
        e.name = "ha_call_service";
        e.toolset = "homeassistant";
        e.description = "Call a Home Assistant service";
        e.emoji = "\xf0\x9f\x8f\xa0";
        e.check_fn = ha_env_available;
        e.schema = nlohmann::json::parse(R"JSON({
            "type": "object",
            "properties": {
                "domain": {
                    "type": "string",
                    "description": "Service domain (e.g. light)"
                },
                "service": {
                    "type": "string",
                    "description": "Service name (e.g. turn_on)"
                },
                "entity_id": {
                    "type": "string",
                    "description": "Target entity ID"
                },
                "data": {
                    "type": "object",
                    "description": "Additional service data"
                }
            },
            "required": ["domain", "service", "entity_id"]
        })JSON");

        e.handler = [](const nlohmann::json& args,
                       const ToolContext& /*ctx*/) -> std::string {
            for (const char* key : {"domain", "service", "entity_id"}) {
                if (!args.contains(key) || !args[key].is_string()) {
                    return tool_error(std::string("missing required parameter: ") + key);
                }
            }
            // TODO(phase-9): wire cpr
            auto url = ha_url() + "/api/services/" +
                       args["domain"].get<std::string>() + "/" +
                       args["service"].get<std::string>();
            (void)url;
            (void)ha_token();
            return tool_error("HTTP transport not available");
        };

        registry.register_tool(std::move(e));
    }
}

}  // namespace hermes::tools
