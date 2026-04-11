// Message / ContentBlock / ToolCall types with OpenAI and Anthropic
// serialization.  Content blocks preserve Anthropic cache_control markers so
// prompt-cache injection (see prompt_cache.hpp) survives round-trips.
#pragma once

#include <nlohmann/json.hpp>

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace hermes::llm {

enum class Role {
    System,
    User,
    Assistant,
    Tool,
};

std::string role_to_string(Role r);
Role role_from_string(std::string_view s);

struct ToolCall {
    std::string id;
    std::string name;
    nlohmann::json arguments;  // parsed JSON object
};

struct ContentBlock {
    // Supported types: "text", "tool_use", "tool_result", "thinking".
    std::string type;
    std::string text;
    // Provider-specific extras: tool_use_id, input/output shape, etc.
    nlohmann::json extra;
    // Anthropic prompt cache marker, e.g. {"type":"ephemeral"}.
    std::optional<nlohmann::json> cache_control;
};

struct Message {
    Role role{Role::User};
    std::string content_text;                 // simple textual content
    std::vector<ContentBlock> content_blocks; // multi-part content
    std::vector<ToolCall> tool_calls;         // assistant tool invocations
    std::optional<std::string> tool_call_id;  // for Role::Tool replies
    std::optional<std::string> reasoning;     // extended thinking / reasoning
    // Optional top-level cache_control (Anthropic sometimes accepts this on
    // tool-role messages).  For user/assistant we always push the marker
    // onto the last content block instead.
    std::optional<nlohmann::json> cache_control;

    nlohmann::json to_openai() const;
    nlohmann::json to_anthropic() const;

    static Message from_openai(const nlohmann::json& obj);
    static Message from_anthropic(const nlohmann::json& obj);
};

}  // namespace hermes::llm
