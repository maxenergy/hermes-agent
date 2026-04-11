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

CompletionResponse AnthropicClient::complete(const CompletionRequest& req) {
    if (req.stream) {
        // TODO(phase-4): SSE streaming
        throw std::runtime_error("streaming not yet implemented");
    }

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
