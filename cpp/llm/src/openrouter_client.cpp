// OpenRouter client — identical wire format to OpenAI chat-completions but
// adds HTTP-Referer and X-Title so OpenRouter can attribute usage.  We
// hardcode the base URL because OpenRouter doesn't offer path overrides.
#include "hermes/llm/openrouter_client.hpp"

#include <nlohmann/json.hpp>

#include <stdexcept>
#include <utility>

namespace hermes::llm {

using nlohmann::json;

namespace {

json build_body(const CompletionRequest& req) {
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
    if (req.extra.is_object()) {
        for (auto it = req.extra.begin(); it != req.extra.end(); ++it) {
            body[it.key()] = it.value();
        }
    }
    return body;
}

}  // namespace

OpenRouterClient::OpenRouterClient(HttpTransport* transport,
                                   std::string api_key,
                                   std::string referer,
                                   std::string title)
    : transport_(transport),
      api_key_(std::move(api_key)),
      referer_(std::move(referer)),
      title_(std::move(title)) {
    if (!transport_) {
        throw std::invalid_argument("OpenRouterClient: transport must not be null");
    }
}

CompletionResponse OpenRouterClient::complete(const CompletionRequest& req) {
    if (req.stream) {
        throw std::runtime_error("streaming not yet implemented");
    }

    const json body = build_body(req);
    std::unordered_map<std::string, std::string> headers = {
        {"Authorization", "Bearer " + api_key_},
        {"Content-Type", "application/json"},
        {"HTTP-Referer", referer_},
        {"X-Title", title_},
    };
    const auto resp = transport_->post_json(
        "https://openrouter.ai/api/v1/chat/completions", headers, body.dump());
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
    if (!parsed.contains("choices") || !parsed["choices"].is_array() ||
        parsed["choices"].empty()) {
        throw ApiError(502, "empty choices", provider_name());
    }
    const auto& choice = parsed["choices"][0];
    if (choice.contains("message")) {
        out.assistant_message = Message::from_openai(choice["message"]);
    }
    out.finish_reason = choice.value("finish_reason", "");
    if (parsed.contains("usage")) {
        out.usage = normalize_openai_usage(parsed["usage"]);
    }
    return out;
}

}  // namespace hermes::llm
