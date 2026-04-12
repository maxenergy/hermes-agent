#include "hermes/tools/clarify_tool.hpp"

#include "hermes/tools/registry.hpp"

#include <nlohmann/json.hpp>

#include <mutex>
#include <string>
#include <vector>

namespace hermes::tools {

namespace {

std::mutex& clarify_mu() {
    static std::mutex mu;
    return mu;
}

ClarifyCallback& stored_callback() {
    static ClarifyCallback cb;
    return cb;
}

}  // namespace

void set_clarify_callback(ClarifyCallback cb) {
    std::lock_guard<std::mutex> lk(clarify_mu());
    stored_callback() = std::move(cb);
}

void clear_clarify_callback() {
    std::lock_guard<std::mutex> lk(clarify_mu());
    stored_callback() = nullptr;
}

void register_clarify_tools(ToolRegistry& registry) {
    ToolEntry e;
    e.name = "clarify";
    e.toolset = "clarify";
    e.description = "Ask the user a clarifying question";
    e.emoji = "\xe2\x9d\x93";  // question mark
    e.schema = nlohmann::json::parse(R"JSON({
        "type": "object",
        "properties": {
            "question": {
                "type": "string",
                "description": "The question to ask the user"
            },
            "choices": {
                "type": "array",
                "items": {"type": "string"},
                "maxItems": 4,
                "description": "Optional list of choices (max 4)"
            }
        },
        "required": ["question"]
    })JSON");

    e.handler = [](const nlohmann::json& args,
                   const ToolContext& /*ctx*/) -> std::string {
        if (!args.contains("question") || !args["question"].is_string()) {
            return tool_error("missing required parameter: question");
        }
        const auto question = args["question"].get<std::string>();

        std::vector<std::string> choices;
        if (args.contains("choices") && args["choices"].is_array()) {
            for (const auto& c : args["choices"]) {
                if (c.is_string()) {
                    choices.push_back(c.get<std::string>());
                }
            }
            if (choices.size() > 4) {
                return tool_error("max 4 choices allowed");
            }
        }

        std::lock_guard<std::mutex> lk(clarify_mu());
        auto& cb = stored_callback();
        if (!cb) {
            return tool_error("no clarify callback registered");
        }
        auto answer = cb(question, choices);
        return tool_result({{"answer", answer}});
    };

    registry.register_tool(std::move(e));
}

}  // namespace hermes::tools
