#include "hermes/tools/session_search_tool.hpp"

#include "hermes/state/session_db.hpp"
#include "hermes/tools/registry.hpp"

#include <nlohmann/json.hpp>

#include <string>

namespace hermes::tools {

void register_session_search_tools(ToolRegistry& registry) {
    ToolEntry e;
    e.name = "session_search";
    e.toolset = "search";
    e.description = "Full-text search over past sessions";
    e.emoji = "\xf0\x9f\x94\x8d";  // magnifying glass
    e.schema = nlohmann::json::parse(R"JSON({
        "type": "object",
        "properties": {
            "query": {
                "type": "string",
                "description": "Search query"
            },
            "limit": {
                "type": "integer",
                "default": 3,
                "description": "Maximum number of results"
            }
        },
        "required": ["query"]
    })JSON");

    e.handler = [](const nlohmann::json& args,
                   const ToolContext& /*ctx*/) -> std::string {
        if (!args.contains("query") || !args["query"].is_string()) {
            return tool_error("missing required parameter: query");
        }
        auto query = args["query"].get<std::string>();
        int limit = args.value("limit", 3);

        if (query.empty()) {
            return tool_result(
                {{"results", nlohmann::json::array()}, {"count", 0}});
        }

        hermes::state::SessionDB db;
        auto hits = db.fts_search(query, limit);

        nlohmann::json results = nlohmann::json::array();
        for (const auto& h : hits) {
            results.push_back({{"session_id", h.session_id},
                               {"snippet", h.snippet},
                               {"score", h.score}});
        }
        return tool_result(
            {{"results", results},
             {"count", static_cast<int>(results.size())}});
    };

    registry.register_tool(std::move(e));
}

}  // namespace hermes::tools
