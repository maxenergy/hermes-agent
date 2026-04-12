// OpenRouter client — identical wire format to OpenAI chat-completions but
// adds HTTP-Referer and X-Title so OpenRouter can attribute usage.  We
// hardcode the base URL because OpenRouter doesn't offer path overrides.
#include "hermes/llm/openrouter_client.hpp"

#include <nlohmann/json.hpp>

#include <stdexcept>
#include <string>
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

namespace {

std::string extract_or_sse_data(const std::string& line) {
    std::string trimmed = line;
    while (!trimmed.empty() &&
           (trimmed.back() == '\r' || trimmed.back() == '\n' ||
            trimmed.back() == ' ')) {
        trimmed.pop_back();
    }
    if (trimmed.rfind("data: ", 0) != 0) return {};
    return trimmed.substr(6);
}

CompletionResponse parse_openrouter_stream(
    HttpTransport* transport,
    const std::string& url,
    const std::unordered_map<std::string, std::string>& headers,
    const std::string& body_str) {

    std::string accumulated_content;
    std::string finish_reason;
    CanonicalUsage usage;
    std::vector<ToolCall> tool_calls;
    json raw_last;

    transport->post_json_stream(url, headers, body_str,
        [&](const std::string& chunk) {
            auto data = extract_or_sse_data(chunk);
            if (data.empty() || data == "[DONE]") return;

            json delta_obj;
            try { delta_obj = json::parse(data); } catch (...) { return; }
            raw_last = delta_obj;

            if (!delta_obj.contains("choices") ||
                !delta_obj["choices"].is_array() ||
                delta_obj["choices"].empty()) {
                if (delta_obj.contains("usage"))
                    usage = normalize_openai_usage(delta_obj["usage"]);
                return;
            }

            const auto& choice = delta_obj["choices"][0];
            if (choice.contains("finish_reason") &&
                !choice["finish_reason"].is_null())
                finish_reason = choice["finish_reason"].get<std::string>();
            if (!choice.contains("delta")) return;
            const auto& delta = choice["delta"];

            if (delta.contains("content") && delta["content"].is_string())
                accumulated_content += delta["content"].get<std::string>();

            if (delta.contains("tool_calls") && delta["tool_calls"].is_array()) {
                for (const auto& tc_delta : delta["tool_calls"]) {
                    auto idx = tc_delta.value("index", 0);
                    while (static_cast<int>(tool_calls.size()) <= idx)
                        tool_calls.emplace_back();
                    if (tc_delta.contains("id"))
                        tool_calls[idx].id = tc_delta["id"].get<std::string>();
                    if (tc_delta.contains("function")) {
                        const auto& fn = tc_delta["function"];
                        if (fn.contains("name"))
                            tool_calls[idx].name = fn["name"].get<std::string>();
                        if (fn.contains("arguments")) {
                            auto& args = tool_calls[idx].arguments;
                            if (!args.is_string()) args = std::string{};
                            args = args.get<std::string>() +
                                   fn["arguments"].get<std::string>();
                        }
                    }
                }
            }
            if (delta_obj.contains("usage"))
                usage = normalize_openai_usage(delta_obj["usage"]);
        });

    for (auto& tc : tool_calls) {
        if (tc.arguments.is_string()) {
            auto raw = tc.arguments.get<std::string>();
            try { tc.arguments = json::parse(raw); } catch (...) { tc.arguments = raw; }
        }
    }

    CompletionResponse out;
    out.raw = raw_last;
    out.finish_reason = finish_reason;
    out.usage = usage;
    out.assistant_message.role = Role::Assistant;
    out.assistant_message.content_text = accumulated_content;
    out.assistant_message.tool_calls = std::move(tool_calls);
    return out;
}

}  // namespace

CompletionResponse OpenRouterClient::complete(const CompletionRequest& req) {
    json body = build_body(req);
    std::unordered_map<std::string, std::string> headers = {
        {"Authorization", "Bearer " + api_key_},
        {"Content-Type", "application/json"},
        {"HTTP-Referer", referer_},
        {"X-Title", title_},
    };

    if (req.stream) {
        body["stream"] = true;
        return parse_openrouter_stream(
            transport_, "https://openrouter.ai/api/v1/chat/completions",
            headers, body.dump());
    }

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
