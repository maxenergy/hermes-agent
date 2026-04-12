#include "hermes/tools/memory_tool.hpp"

#include "hermes/state/memory_store.hpp"
#include "hermes/tools/registry.hpp"

#include <nlohmann/json.hpp>

#include <string>

namespace hermes::tools {

// Not in anonymous namespace — avoids false -Wunused-function with lambdas.
static hermes::state::MemoryFile parse_file_param(const nlohmann::json& args) {
    std::string file = "agent";
    if (args.contains("file") && args["file"].is_string()) {
        file = args["file"].get<std::string>();
    }
    if (file == "user") {
        return hermes::state::MemoryFile::User;
    }
    return hermes::state::MemoryFile::Agent;
}

void register_memory_tools(ToolRegistry& registry) {
    ToolEntry e;
    e.name = "memory";
    e.toolset = "memory";
    e.description = "Manage persistent memory entries (add/read/replace/remove)";
    e.emoji = "\xf0\x9f\xa7\xa0";  // brain emoji
    e.schema = nlohmann::json::parse(R"JSON({
        "type": "object",
        "properties": {
            "action": {
                "type": "string",
                "enum": ["add", "read", "replace", "remove"],
                "description": "The memory operation to perform"
            },
            "file": {
                "type": "string",
                "enum": ["agent", "user"],
                "default": "agent",
                "description": "Which memory file to operate on"
            },
            "entry": {
                "type": "string",
                "description": "The entry text (for add)"
            },
            "needle": {
                "type": "string",
                "description": "Substring to match (for replace/remove)"
            },
            "replacement": {
                "type": "string",
                "description": "Replacement text (for replace)"
            }
        },
        "required": ["action"]
    })JSON");

    e.handler = [](const nlohmann::json& args,
                   const ToolContext& /*ctx*/) -> std::string {
        if (!args.contains("action") || !args["action"].is_string()) {
            return tool_error("missing required parameter: action");
        }
        const auto action = args["action"].get<std::string>();
        auto which = parse_file_param(args);
        hermes::state::MemoryStore store;

        if (action == "add") {
            if (!args.contains("entry") || !args["entry"].is_string()) {
                return tool_error("add requires 'entry' parameter");
            }
            store.add(which, args["entry"].get<std::string>());
            auto entries = store.read_all(which);
            return tool_result({{"added", true},
                                {"count", static_cast<int>(entries.size())}});
        }
        if (action == "read") {
            auto entries = store.read_all(which);
            nlohmann::json arr = nlohmann::json::array();
            for (const auto& entry : entries) {
                arr.push_back(entry);
            }
            return tool_result(
                {{"entries", arr},
                 {"count", static_cast<int>(entries.size())}});
        }
        if (action == "replace") {
            if (!args.contains("needle") || !args["needle"].is_string()) {
                return tool_error("replace requires 'needle' parameter");
            }
            if (!args.contains("replacement") ||
                !args["replacement"].is_string()) {
                return tool_error("replace requires 'replacement' parameter");
            }
            store.replace(which, args["needle"].get<std::string>(),
                          args["replacement"].get<std::string>());
            return tool_result({{"replaced", true}});
        }
        if (action == "remove") {
            if (!args.contains("needle") || !args["needle"].is_string()) {
                return tool_error("remove requires 'needle' parameter");
            }
            store.remove(which, args["needle"].get<std::string>());
            return tool_result({{"removed", true}});
        }
        return tool_error("invalid action: " + action +
                          "; expected add|read|replace|remove");
    };

    registry.register_tool(std::move(e));
}

}  // namespace hermes::tools
