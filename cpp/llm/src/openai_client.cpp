// OpenAI chat-completions client.
//
// Wire format for POST /v1/chat/completions:
//   { "model": "...",
//     "messages": [...],          // see Message::to_openai()
//     "tools": [ {"type":"function",
//                 "function": {"name":..., "description":..., "parameters":...}} ],
//     "temperature": 0.7,
//     "max_tokens": 1024,
//     ... provider-specific extras ... }
#include "hermes/llm/openai_client.hpp"

#include <nlohmann/json.hpp>

#include <stdexcept>
#include <utility>

namespace hermes::llm {

using nlohmann::json;

namespace {

json build_chat_completions_body(const CompletionRequest& req) {
    json body;
    body["model"] = req.model;

    json msgs = json::array();
    for (const auto& m : req.messages) {
        msgs.push_back(m.to_openai());
    }
    body["messages"] = std::move(msgs);

    if (!req.tools.empty()) {
        json tools = json::array();
        for (const auto& t : req.tools) {
            json tool;
            tool["type"] = "function";
            tool["function"]["name"] = t.name;
            tool["function"]["description"] = t.description;
            tool["function"]["parameters"] = t.parameters;
            tools.push_back(std::move(tool));
        }
        body["tools"] = std::move(tools);
    }

    if (req.temperature) body["temperature"] = *req.temperature;
    if (req.max_tokens) body["max_tokens"] = *req.max_tokens;
    if (req.reasoning_effort) body["reasoning_effort"] = *req.reasoning_effort;

    // Merge provider-specific extras last so callers can override.
    if (req.extra.is_object()) {
        for (auto it = req.extra.begin(); it != req.extra.end(); ++it) {
            body[it.key()] = it.value();
        }
    }
    return body;
}

CompletionResponse parse_chat_completions_response(const json& body,
                                                   const std::string& provider) {
    CompletionResponse out;
    out.raw = body;

    if (!body.contains("choices") || !body["choices"].is_array() ||
        body["choices"].empty()) {
        throw ApiError(502,
                       std::string("empty choices in response: ") + body.dump(),
                       provider);
    }
    const auto& choice = body["choices"][0];
    if (choice.contains("message")) {
        out.assistant_message = Message::from_openai(choice["message"]);
    }
    out.finish_reason = choice.value("finish_reason", "");

    if (body.contains("usage")) {
        out.usage = normalize_openai_usage(body["usage"]);
    }
    return out;
}

}  // namespace

OpenAIClient::OpenAIClient(HttpTransport* transport,
                           std::string api_key,
                           std::string base_url)
    : transport_(transport),
      api_key_(std::move(api_key)),
      base_url_(std::move(base_url)) {
    if (!transport_) {
        throw std::invalid_argument("OpenAIClient: transport must not be null");
    }
}

CompletionResponse OpenAIClient::complete(const CompletionRequest& req) {
    if (req.stream) {
        // TODO(phase-4): SSE streaming
        throw std::runtime_error("streaming not yet implemented");
    }

    const json body = build_chat_completions_body(req);
    std::unordered_map<std::string, std::string> headers = {
        {"Authorization", "Bearer " + api_key_},
        {"Content-Type", "application/json"},
    };
    const auto resp = transport_->post_json(
        base_url_ + "/chat/completions", headers, body.dump());
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
    return parse_chat_completions_response(parsed, provider_name());
}

}  // namespace hermes::llm
