// OpenAI + Anthropic message serialization.
//
// OpenAI chat-completions format:
//   { "role": "user|assistant|system|tool",
//     "content": <string> | [<content-part>...],
//     "tool_calls": [ { "id":..., "type":"function",
//                       "function": {"name":..., "arguments":"<json-string>"} } ],
//     "tool_call_id": "..."  // tool-role only
//   }
//
// Anthropic Messages format:
//   - system lives at the top level of the request, not as a message.
//     Message::to_anthropic() still returns a system message for callers
//     that want to inspect it; the AnthropicClient hoists it.
//   - content is always a list of content blocks.
//   - tool_use / tool_result live inside content blocks, not as
//     separate tool_calls arrays.
//   - cache_control is a per-block attribute.
#include "hermes/llm/message.hpp"

#include <stdexcept>
#include <utility>

namespace hermes::llm {

using nlohmann::json;

std::string role_to_string(Role r) {
    switch (r) {
        case Role::System: return "system";
        case Role::User: return "user";
        case Role::Assistant: return "assistant";
        case Role::Tool: return "tool";
    }
    return "user";
}

Role role_from_string(std::string_view s) {
    if (s == "system") return Role::System;
    if (s == "assistant") return Role::Assistant;
    if (s == "tool") return Role::Tool;
    // Map Anthropic's absence of a "tool" role via tool_result to User.
    return Role::User;
}

// ── OpenAI serialization ────────────────────────────────────────────────

namespace {

json content_block_to_openai(const ContentBlock& b) {
    json out;
    if (b.type == "text" || b.type.empty()) {
        out["type"] = "text";
        out["text"] = b.text;
    } else if (b.type == "thinking") {
        // OpenAI doesn't support thinking blocks in chat; emit as text.
        out["type"] = "text";
        out["text"] = b.text;
    } else {
        // tool_use / tool_result / other: preserve extra payload.
        out["type"] = b.type;
        if (!b.text.empty()) out["text"] = b.text;
        if (!b.extra.is_null()) {
            for (auto it = b.extra.begin(); it != b.extra.end(); ++it) {
                out[it.key()] = it.value();
            }
        }
    }
    if (b.cache_control) {
        out["cache_control"] = *b.cache_control;
    }
    return out;
}

json content_block_to_anthropic(const ContentBlock& b) {
    json out;
    if (b.type.empty()) {
        out["type"] = "text";
        out["text"] = b.text;
    } else {
        out["type"] = b.type;
        if (b.type == "text" || b.type == "thinking") {
            out["text"] = b.text;
        }
        if (!b.extra.is_null()) {
            for (auto it = b.extra.begin(); it != b.extra.end(); ++it) {
                out[it.key()] = it.value();
            }
        }
    }
    if (b.cache_control) {
        out["cache_control"] = *b.cache_control;
    }
    return out;
}

ContentBlock content_block_from_json(const json& obj) {
    ContentBlock b;
    if (obj.is_string()) {
        b.type = "text";
        b.text = obj.get<std::string>();
        return b;
    }
    if (!obj.is_object()) return b;
    b.type = obj.value("type", "text");
    b.text = obj.value("text", "");
    // Preserve unknown fields into extra (excluding type/text/cache_control).
    for (auto it = obj.begin(); it != obj.end(); ++it) {
        if (it.key() == "type" || it.key() == "text" || it.key() == "cache_control") {
            continue;
        }
        b.extra[it.key()] = it.value();
    }
    if (obj.contains("cache_control")) {
        b.cache_control = obj["cache_control"];
    }
    return b;
}

}  // namespace

json Message::to_openai() const {
    json out;
    out["role"] = role_to_string(role);

    // Content: prefer content_blocks when present, else content_text.
    if (!content_blocks.empty()) {
        json arr = json::array();
        for (const auto& b : content_blocks) {
            arr.push_back(content_block_to_openai(b));
        }
        out["content"] = std::move(arr);
    } else if (role == Role::Tool) {
        // tool responses must be string content
        out["content"] = content_text;
    } else if (!content_text.empty()) {
        out["content"] = content_text;
    } else {
        out["content"] = nullptr;
    }

    if (role == Role::Assistant && !tool_calls.empty()) {
        json calls = json::array();
        for (const auto& tc : tool_calls) {
            json c;
            c["id"] = tc.id;
            c["type"] = "function";
            c["function"]["name"] = tc.name;
            c["function"]["arguments"] = tc.arguments.dump();
            calls.push_back(std::move(c));
        }
        out["tool_calls"] = std::move(calls);
    }

    if (tool_call_id) {
        out["tool_call_id"] = *tool_call_id;
    }
    if (reasoning) {
        out["reasoning"] = *reasoning;
    }
    return out;
}

json Message::to_anthropic() const {
    json out;
    out["role"] = role_to_string(role);

    json content = json::array();
    // Anthropic requires an array of content blocks even for simple text.
    if (!content_blocks.empty()) {
        for (const auto& b : content_blocks) {
            content.push_back(content_block_to_anthropic(b));
        }
    }
    if (!content_text.empty()) {
        json block;
        block["type"] = "text";
        block["text"] = content_text;
        content.push_back(std::move(block));
    }
    // Assistant tool_calls become tool_use blocks.
    for (const auto& tc : tool_calls) {
        json block;
        block["type"] = "tool_use";
        block["id"] = tc.id;
        block["name"] = tc.name;
        block["input"] = tc.arguments;
        content.push_back(std::move(block));
    }
    // Tool-role replies reference the original tool_use id via content blocks.
    if (role == Role::Tool && tool_call_id) {
        // If the caller already supplied the tool_result block, keep it;
        // otherwise synthesize one from content_text.
        bool has_tool_result = false;
        for (const auto& b : content) {
            if (b.value("type", "") == "tool_result") {
                has_tool_result = true;
                break;
            }
        }
        if (!has_tool_result) {
            json block;
            block["type"] = "tool_result";
            block["tool_use_id"] = *tool_call_id;
            block["content"] = content_text;
            content = json::array();  // replace with single tool_result
            content.push_back(std::move(block));
        }
    }

    out["content"] = std::move(content);
    if (cache_control) {
        out["cache_control"] = *cache_control;
    }
    return out;
}

Message Message::from_openai(const json& obj) {
    Message m;
    m.role = role_from_string(obj.value("role", "user"));
    if (obj.contains("content")) {
        const auto& c = obj["content"];
        if (c.is_string()) {
            m.content_text = c.get<std::string>();
        } else if (c.is_array()) {
            for (const auto& part : c) {
                m.content_blocks.push_back(content_block_from_json(part));
            }
        }
        // null / missing: leave blank
    }
    if (obj.contains("tool_calls") && obj["tool_calls"].is_array()) {
        for (const auto& tc : obj["tool_calls"]) {
            ToolCall call;
            call.id = tc.value("id", "");
            if (tc.contains("function")) {
                const auto& fn = tc["function"];
                call.name = fn.value("name", "");
                const auto args = fn.value("arguments", std::string{});
                if (!args.empty()) {
                    try {
                        call.arguments = json::parse(args);
                    } catch (const std::exception&) {
                        call.arguments = args;  // preserve raw
                    }
                }
            }
            m.tool_calls.push_back(std::move(call));
        }
    }
    if (obj.contains("tool_call_id") && obj["tool_call_id"].is_string()) {
        m.tool_call_id = obj["tool_call_id"].get<std::string>();
    }
    if (obj.contains("reasoning") && obj["reasoning"].is_string()) {
        m.reasoning = obj["reasoning"].get<std::string>();
    }
    return m;
}

Message Message::from_anthropic(const json& obj) {
    Message m;
    m.role = role_from_string(obj.value("role", "assistant"));
    if (obj.contains("content")) {
        const auto& c = obj["content"];
        if (c.is_string()) {
            m.content_text = c.get<std::string>();
        } else if (c.is_array()) {
            for (const auto& part : c) {
                if (!part.is_object()) {
                    m.content_blocks.push_back(content_block_from_json(part));
                    continue;
                }
                const std::string type = part.value("type", "text");
                if (type == "tool_use") {
                    ToolCall call;
                    call.id = part.value("id", "");
                    call.name = part.value("name", "");
                    if (part.contains("input")) {
                        call.arguments = part["input"];
                    }
                    m.tool_calls.push_back(std::move(call));
                } else {
                    m.content_blocks.push_back(content_block_from_json(part));
                }
            }
        }
    }
    if (obj.contains("cache_control")) {
        m.cache_control = obj["cache_control"];
    }
    return m;
}

}  // namespace hermes::llm
