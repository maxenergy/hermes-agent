// Anthropic Messages API client.
//
// POST /v1/messages
//   { "model": "...",
//     "system": <string or [blocks]>,       // top-level, NOT in messages
//     "messages": [ {"role":"user|assistant",
//                    "content":[<content-block>...]} ],
//     "tools": [ {"name":..., "description":..., "input_schema":...} ],
//     "max_tokens": N,                      // required by Anthropic
//     ... }
//
// Response:
//   { "id":..., "type":"message", "role":"assistant",
//     "content": [...], "usage": {"input_tokens":..., ...} }
//
// Applies apply_anthropic_cache_control() when
// req.cache.native_anthropic is true.
#include "hermes/llm/anthropic_client.hpp"

#include "hermes/llm/prompt_cache.hpp"

#include <nlohmann/json.hpp>

#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace hermes::llm {

using nlohmann::json;

namespace {

// Split messages into (system_blocks, non_system) for the Anthropic wire
// format: system is a top-level field, non-system messages go into
// `messages`.
void partition_system(std::vector<Message>& messages,
                      json& system_out,
                      std::vector<Message>& non_system_out) {
    for (auto& m : messages) {
        if (m.role == Role::System) {
            // Anthropic accepts either a plain string or an array of text
            // blocks.  We emit blocks so cache_control survives.
            if (!system_out.is_array()) {
                system_out = json::array();
            }
            if (!m.content_blocks.empty()) {
                for (const auto& b : m.content_blocks) {
                    json block;
                    block["type"] = b.type.empty() ? "text" : b.type;
                    block["text"] = b.text;
                    if (b.cache_control) {
                        block["cache_control"] = *b.cache_control;
                    }
                    system_out.push_back(std::move(block));
                }
            } else if (!m.content_text.empty()) {
                json block;
                block["type"] = "text";
                block["text"] = m.content_text;
                system_out.push_back(std::move(block));
            }
        } else {
            non_system_out.push_back(m);
        }
    }
}

}  // namespace

AnthropicClient::AnthropicClient(HttpTransport* transport,
                                 std::string api_key,
                                 std::string base_url)
    : transport_(transport),
      api_key_(std::move(api_key)),
      base_url_(std::move(base_url)) {
    if (!transport_) {
        throw std::invalid_argument("AnthropicClient: transport must not be null");
    }
}

namespace {

/// Parse an SSE line to extract the data payload.
std::string extract_anthropic_sse_data(const std::string& line) {
    std::string trimmed = line;
    while (!trimmed.empty() &&
           (trimmed.back() == '\r' || trimmed.back() == '\n' ||
            trimmed.back() == ' ')) {
        trimmed.pop_back();
    }
    if (trimmed.rfind("data: ", 0) != 0) return {};
    return trimmed.substr(6);
}

/// Parse an SSE event type line.
std::string extract_anthropic_event_type(const std::string& line) {
    std::string trimmed = line;
    while (!trimmed.empty() &&
           (trimmed.back() == '\r' || trimmed.back() == '\n' ||
            trimmed.back() == ' ')) {
        trimmed.pop_back();
    }
    if (trimmed.rfind("event: ", 0) != 0) return {};
    return trimmed.substr(7);
}

CompletionResponse parse_anthropic_stream(
    HttpTransport* transport,
    const std::string& url,
    const std::unordered_map<std::string, std::string>& headers,
    const std::string& body_str) {

    std::string accumulated_content;
    std::string finish_reason;
    CanonicalUsage usage;
    std::vector<ToolCall> tool_calls;
    nlohmann::json raw_last;
    std::string current_event_type;
    // Track current content block for tool_use accumulation.
    int current_block_index = -1;
    std::string current_tool_input_json;

    transport->post_json_stream(url, headers, body_str,
        [&](const std::string& chunk) {
            // Check for event type lines.
            auto evt = extract_anthropic_event_type(chunk);
            if (!evt.empty()) {
                current_event_type = evt;
                return;
            }

            auto data = extract_anthropic_sse_data(chunk);
            if (data.empty()) return;

            nlohmann::json obj;
            try {
                obj = nlohmann::json::parse(data);
            } catch (...) {
                return;
            }
            raw_last = obj;

            auto type = obj.value("type", "");

            if (type == "message_start") {
                // Extract usage from initial message.
                if (obj.contains("message") &&
                    obj["message"].contains("usage")) {
                    usage = normalize_anthropic_usage(obj["message"]["usage"]);
                }
            } else if (type == "content_block_start") {
                current_block_index = obj.value("index", -1);
                if (obj.contains("content_block")) {
                    auto block_type = obj["content_block"].value("type", "");
                    if (block_type == "tool_use") {
                        ToolCall tc;
                        tc.id = obj["content_block"].value("id", "");
                        tc.name = obj["content_block"].value("name", "");
                        tc.arguments = nlohmann::json::object();
                        tool_calls.push_back(std::move(tc));
                        current_tool_input_json.clear();
                    }
                }
            } else if (type == "content_block_delta") {
                if (obj.contains("delta")) {
                    auto delta_type = obj["delta"].value("type", "");
                    if (delta_type == "text_delta") {
                        accumulated_content +=
                            obj["delta"].value("text", "");
                    } else if (delta_type == "input_json_delta") {
                        current_tool_input_json +=
                            obj["delta"].value("partial_json", "");
                    }
                }
            } else if (type == "content_block_stop") {
                // Finalize tool call input if we were accumulating JSON.
                if (!current_tool_input_json.empty() && !tool_calls.empty()) {
                    try {
                        tool_calls.back().arguments =
                            nlohmann::json::parse(current_tool_input_json);
                    } catch (...) {
                        tool_calls.back().arguments = current_tool_input_json;
                    }
                    current_tool_input_json.clear();
                }
            } else if (type == "message_delta") {
                if (obj.contains("delta")) {
                    finish_reason = obj["delta"].value("stop_reason", "");
                }
                if (obj.contains("usage")) {
                    // Merge output token count from delta.
                    auto delta_usage = normalize_anthropic_usage(obj["usage"]);
                    usage.output_tokens = delta_usage.output_tokens;
                }
            } else if (type == "message_stop") {
                // Stream complete.
            }
        });

    CompletionResponse out;
    out.raw = raw_last;
    out.finish_reason = finish_reason;
    out.usage = usage;
    out.assistant_message.role = Role::Assistant;
    if (!accumulated_content.empty()) {
        ContentBlock block;
        block.type = "text";
        block.text = accumulated_content;
        out.assistant_message.content_blocks.push_back(std::move(block));
    }
    out.assistant_message.tool_calls = std::move(tool_calls);
    return out;
}

}  // namespace

CompletionResponse AnthropicClient::complete(const CompletionRequest& req) {
    // Work on a copy so the caller's messages are not mutated by cache
    // injection.  apply_anthropic_cache_control mutates in place per
    // spec, so we need a local vector to own the changes.
    std::vector<Message> messages = req.messages;
    if (req.cache.native_anthropic) {
        apply_anthropic_cache_control(messages, req.cache);
    }

    json system_field;
    std::vector<Message> non_system;
    partition_system(messages, system_field, non_system);

    json body;
    body["model"] = req.model;

    if (!system_field.is_null() && !system_field.empty()) {
        body["system"] = system_field;
    }

    json msgs = json::array();
    for (const auto& m : non_system) {
        msgs.push_back(m.to_anthropic());
    }
    body["messages"] = std::move(msgs);

    // Anthropic REQUIRES max_tokens — default to a conservative 4096.
    body["max_tokens"] = req.max_tokens.value_or(4096);

    if (req.temperature) body["temperature"] = *req.temperature;

    if (!req.tools.empty()) {
        json tools = json::array();
        for (const auto& t : req.tools) {
            json tool;
            tool["name"] = t.name;
            tool["description"] = t.description;
            tool["input_schema"] = t.parameters;
            tools.push_back(std::move(tool));
        }
        body["tools"] = std::move(tools);
    }

    if (req.extra.is_object()) {
        for (auto it = req.extra.begin(); it != req.extra.end(); ++it) {
            body[it.key()] = it.value();
        }
    }

    std::unordered_map<std::string, std::string> headers = {
        {"x-api-key", api_key_},
        {"anthropic-version", "2023-06-01"},
        {"Content-Type", "application/json"},
    };
    // Opt into extended prompt caching TTL if requested.
    if (req.cache.native_anthropic && req.cache.cache_ttl == "1h") {
        headers["anthropic-beta"] = "extended-cache-ttl-2025-04-11";
    }

    if (req.stream) {
        body["stream"] = true;
        return parse_anthropic_stream(
            transport_, base_url_ + "/messages",
            headers, body.dump());
    }

    const auto resp = transport_->post_json(
        base_url_ + "/messages", headers, body.dump());
    if (resp.status_code < 200 || resp.status_code >= 300) {
        throw ApiError(resp.status_code, resp.body, provider_name());
    }
    json parsed;
    try {
        parsed = json::parse(resp.body);
    } catch (const std::exception& e) {
        throw ApiError(resp.status_code,
                       std::string("invalid JSON response: ") + e.what(),
                       provider_name());
    }

    CompletionResponse out;
    out.raw = parsed;
    out.assistant_message = Message::from_anthropic(parsed);
    out.assistant_message.role = Role::Assistant;
    out.finish_reason = parsed.value("stop_reason", "");
    if (parsed.contains("usage")) {
        out.usage = normalize_anthropic_usage(parsed["usage"]);
    }
    return out;
}

}  // namespace hermes::llm
