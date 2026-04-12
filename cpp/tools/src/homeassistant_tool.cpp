#include "hermes/tools/homeassistant_tool.hpp"

#include "hermes/tools/registry.hpp"

#include <nlohmann/json.hpp>

#include <cassert>
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

hermes::llm::HttpTransport* get_ha_transport() {
    return hermes::llm::get_default_transport();
}

std::unordered_map<std::string, std::string> ha_headers() {
    std::unordered_map<std::string, std::string> h;
    h["Authorization"] = "Bearer " + ha_token();
    h["Content-Type"] = "application/json";
    return h;
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
            auto* transport = get_ha_transport();
            // CurlTransport is always available when built with libcurl.
            assert(transport && "HTTP transport should always be available");

            auto url = ha_url() + "/api/states";
            auto resp = transport->get(url, ha_headers());

            if (resp.status_code != 200) {
                return tool_error("Home Assistant API error",
                                  {{"status", resp.status_code},
                                   {"body", resp.body}});
            }

            auto body = nlohmann::json::parse(resp.body, nullptr, false);
            if (body.is_discarded()) {
                return tool_error("malformed response from Home Assistant");
            }

            nlohmann::json entities = nlohmann::json::array();
            for (const auto& state : body) {
                nlohmann::json ent;
                ent["entity_id"] = state.value("entity_id", "");
                ent["state"] = state.value("state", "");
                if (state.contains("attributes") &&
                    state["attributes"].contains("friendly_name")) {
                    ent["friendly_name"] =
                        state["attributes"]["friendly_name"];
                }
                entities.push_back(std::move(ent));
            }

            nlohmann::json result;
            result["entities"] = std::move(entities);
            return tool_result(result);
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

            auto* transport = get_ha_transport();
            // CurlTransport is always available when built with libcurl.
            assert(transport && "HTTP transport should always be available");

            auto entity_id = args["entity_id"].get<std::string>();
            auto url = ha_url() + "/api/states/" + entity_id;
            auto resp = transport->get(url, ha_headers());

            if (resp.status_code != 200) {
                return tool_error("Home Assistant API error",
                                  {{"status", resp.status_code},
                                   {"body", resp.body}});
            }

            auto body = nlohmann::json::parse(resp.body, nullptr, false);
            if (body.is_discarded()) {
                return tool_error("malformed response from Home Assistant");
            }

            nlohmann::json result;
            result["entity_id"] = body.value("entity_id", "");
            result["state"] = body.value("state", "");
            if (body.contains("attributes")) {
                result["attributes"] = body["attributes"];
            }
            return tool_result(result);
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
            auto* transport = get_ha_transport();
            // CurlTransport is always available when built with libcurl.
            assert(transport && "HTTP transport should always be available");

            auto url = ha_url() + "/api/services";
            auto resp = transport->get(url, ha_headers());

            if (resp.status_code != 200) {
                return tool_error("Home Assistant API error",
                                  {{"status", resp.status_code},
                                   {"body", resp.body}});
            }

            auto body = nlohmann::json::parse(resp.body, nullptr, false);
            if (body.is_discarded()) {
                return tool_error("malformed response from Home Assistant");
            }

            nlohmann::json result;
            result["services"] = body;
            return tool_result(result);
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

            auto* transport = get_ha_transport();
            // CurlTransport is always available when built with libcurl.
            assert(transport && "HTTP transport should always be available");

            auto url = ha_url() + "/api/services/" +
                       args["domain"].get<std::string>() + "/" +
                       args["service"].get<std::string>();

            nlohmann::json req_body;
            req_body["entity_id"] = args["entity_id"].get<std::string>();
            if (args.contains("data") && args["data"].is_object()) {
                for (auto& [k, v] : args["data"].items()) {
                    req_body[k] = v;
                }
            }

            auto resp = transport->post_json(url, ha_headers(), req_body.dump());

            if (resp.status_code != 200) {
                return tool_error("Home Assistant API error",
                                  {{"status", resp.status_code},
                                   {"body", resp.body}});
            }

            auto body = nlohmann::json::parse(resp.body, nullptr, false);
            if (body.is_discarded()) {
                return tool_error("malformed response from Home Assistant");
            }

            nlohmann::json result;
            result["success"] = true;
            result["response"] = body;
            return tool_result(result);
        };

        registry.register_tool(std::move(e));
    }
}

}  // namespace hermes::tools
