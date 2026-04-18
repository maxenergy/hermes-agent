#include <cstdio>
#include <cstdlib>
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
#include <string>
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
    if (req.reasoning_effort) {
        // Some endpoints (Bailian Coding Plan) require reasoning_effort to
        // be a string ("low"|"medium"|"high") or an object {effort:...},
        // not a raw integer. Map 0..3 → string.
        static constexpr const char* kEffortNames[] = {"none", "low",
                                                      "medium", "high"};
        int level = *req.reasoning_effort;
        if (level < 0) level = 0;
        if (level > 3) level = 3;
        body["reasoning_effort"] = kEffortNames[level];
    }

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

namespace {

/// Parse an SSE line to extract the data payload.  Returns empty if not
/// a "data: " line or if it's the terminal "data: [DONE]" marker.
/// Returns "[DONE]" for the terminal marker so the caller can stop.
std::string extract_sse_data(const std::string& line) {
    // Trim trailing whitespace (\r\n).
    std::string trimmed = line;
    while (!trimmed.empty() &&
           (trimmed.back() == '\r' || trimmed.back() == '\n' ||
            trimmed.back() == ' ')) {
        trimmed.pop_back();
    }
    if (trimmed.rfind("data: ", 0) != 0) return {};
    return trimmed.substr(6);
}

CompletionResponse parse_openai_stream(
    HttpTransport* transport,
    const std::string& url,
    const std::unordered_map<std::string, std::string>& headers,
    const std::string& body_str,
    const std::string& /*provider*/) {

    std::string accumulated_content;
    std::string accumulated_reasoning;
    std::string finish_reason;
    CanonicalUsage usage;
    // Track tool calls by index.
    std::vector<ToolCall> tool_calls;
    json raw_last;

    transport->post_json_stream(url, headers, body_str,
        [&](const std::string& chunk) {
            auto data = extract_sse_data(chunk);
            if (data.empty()) return;
            if (data == "[DONE]") return;

            json delta_obj;
            try {
                delta_obj = json::parse(data);
            } catch (...) {
                return;
            }
            raw_last = delta_obj;

            if (!delta_obj.contains("choices") ||
                !delta_obj["choices"].is_array() ||
                delta_obj["choices"].empty()) {
                // Could be a usage-only chunk.
                if (delta_obj.contains("usage")) {
                    usage = normalize_openai_usage(delta_obj["usage"]);
                }
                return;
            }

            const auto& choice = delta_obj["choices"][0];
            if (choice.contains("finish_reason") &&
                !choice["finish_reason"].is_null()) {
                finish_reason = choice["finish_reason"].get<std::string>();
            }
            if (!choice.contains("delta")) return;
            const auto& delta = choice["delta"];

            // Accumulate content tokens.
            if (delta.contains("content") && delta["content"].is_string()) {
                accumulated_content += delta["content"].get<std::string>();
            }
            // Accumulate reasoning_content (thinking models — DashScope/Qwen,
            // DeepSeek, etc. send reasoning before content).
            if (delta.contains("reasoning_content") &&
                delta["reasoning_content"].is_string()) {
                accumulated_reasoning +=
                    delta["reasoning_content"].get<std::string>();
            }

            // Accumulate tool calls.
            if (delta.contains("tool_calls") && delta["tool_calls"].is_array()) {
                for (const auto& tc_delta : delta["tool_calls"]) {
                    auto idx = tc_delta.value("index", 0);
                    while (static_cast<int>(tool_calls.size()) <= idx) {
                        tool_calls.emplace_back();
                    }
                    if (tc_delta.contains("id")) {
                        tool_calls[idx].id = tc_delta["id"].get<std::string>();
                    }
                    if (tc_delta.contains("function")) {
                        const auto& fn = tc_delta["function"];
                        if (fn.contains("name")) {
                            tool_calls[idx].name = fn["name"].get<std::string>();
                        }
                        if (fn.contains("arguments")) {
                            // Arguments come as string fragments — accumulate.
                            auto& args = tool_calls[idx].arguments;
                            if (!args.is_string()) args = std::string{};
                            args = args.get<std::string>() +
                                   fn["arguments"].get<std::string>();
                        }
                    }
                }
            }

            // Usage in stream chunks (OpenAI includes it with stream_options).
            if (delta_obj.contains("usage")) {
                usage = normalize_openai_usage(delta_obj["usage"]);
            }
        });

    // Parse accumulated tool call argument strings into JSON.
    for (auto& tc : tool_calls) {
        if (tc.arguments.is_string()) {
            auto raw = tc.arguments.get<std::string>();
            try {
                tc.arguments = json::parse(raw);
            } catch (...) {
                tc.arguments = raw;
            }
        }
    }

    CompletionResponse out;
    out.raw = raw_last;
    out.finish_reason = finish_reason;
    out.usage = usage;
    out.assistant_message.role = Role::Assistant;
    out.assistant_message.content_text = accumulated_content;
    if (!accumulated_reasoning.empty()) {
        out.assistant_message.reasoning = accumulated_reasoning;
    }
    out.assistant_message.tool_calls = std::move(tool_calls);
    return out;
}

/// Detect a Codex Responses-API "intermediate ack" — a heartbeat-shaped
/// response with no content, no tool_calls, no reasoning, and no
/// finish_reason.  The caller re-issues the request (up to a cap) so the
/// agent loop doesn't bail out with an empty turn.
///
/// Guardrails (must ALL match to treat as ack):
///   - provider is codex-shaped (name/base_url contains "codex")
///   - assistant content_text is empty/whitespace (and no content_blocks)
///   - tool_calls is empty
///   - reasoning is unset or empty
///   - finish_reason is empty (non-ack real responses carry "stop",
///     "tool_calls", "length", etc.)
///
/// Notably we do NOT treat a response with tool_calls but empty content as
/// ack — that's a legitimate tool-calling turn.
bool is_codex_intermediate_ack(const CompletionResponse& r,
                               const std::string& provider,
                               const std::string& base_url) {
    // Provider gate — matches Python's restriction to openai-codex.
    auto contains_codex = [](const std::string& s) {
        for (size_t i = 0; i + 5 <= s.size(); ++i) {
            char buf[6] = {s[i], s[i + 1], s[i + 2], s[i + 3], s[i + 4], 0};
            for (char& c : buf) {
                c = static_cast<char>(
                    std::tolower(static_cast<unsigned char>(c)));
            }
            if (std::string_view(buf, 5) == "codex") return true;
        }
        return false;
    };
    if (!contains_codex(provider) && !contains_codex(base_url)) return false;

    // Content empty (whitespace-only counts as empty).
    const auto& msg = r.assistant_message;
    bool content_empty = true;
    for (char c : msg.content_text) {
        if (!std::isspace(static_cast<unsigned char>(c))) {
            content_empty = false;
            break;
        }
    }
    if (!content_empty) return false;
    if (!msg.content_blocks.empty()) return false;

    // Must have no tool_calls — a real tool-calling turn is not an ack.
    if (!msg.tool_calls.empty()) return false;

    // Reasoning absent or empty.
    if (msg.reasoning && !msg.reasoning->empty()) return false;

    // finish_reason empty means the stream/JSON didn't actually terminate —
    // the classic heartbeat shape.
    if (!r.finish_reason.empty()) return false;

    return true;
}

}  // namespace

CompletionResponse OpenAIClient::complete(const CompletionRequest& req) {
    json body = build_chat_completions_body(req);
    std::unordered_map<std::string, std::string> headers = {
        {"Authorization", "Bearer " + api_key_},
        {"Content-Type", "application/json"},
    };
    for (const auto& [k, v] : extra_headers_) headers[k] = v;

    const bool stream = req.stream || force_stream_;
    if (stream) {
        body["stream"] = true;
        // Request usage info in stream mode.
        body["stream_options"] = {{"include_usage", true}};
        if (headers.find("User-Agent") == headers.end()) {
            headers["User-Agent"] = "QwenCode/0.14.3 (linux; x64)";
        }
    } else if (headers.find("User-Agent") == headers.end()) {
        // Some OpenAI-compatible endpoints (e.g. Bailian Coding Plan at
        // coding.dashscope.aliyuncs.com) reject default libcurl UAs with
        // "Coding Plan is currently only available for Coding Agents".
        // Send a Qwen Code-shaped UA by default; override via extra_headers.
        headers["User-Agent"] = "QwenCode/0.14.3 (linux; x64)";
    }

    const std::string body_str = body.dump();
    const std::string url = base_url_ + "/chat/completions";

    // Up to 3 intermediate-ack retries (Codex-only path — see
    // is_codex_intermediate_ack above).  A real empty turn only loops when
    // the server keeps sending heartbeats; each retry is a fresh POST with
    // the identical body so cache keys stay stable.
    constexpr int kMaxAckRetries = 3;
    CompletionResponse last_response;
    for (int ack_try = 0; ack_try <= kMaxAckRetries; ++ack_try) {
        if (stream) {
            last_response = parse_openai_stream(
                transport_, url, headers, body_str, provider_name());
        } else {
            const auto resp = transport_->post_json(url, headers, body_str);
            if (std::getenv("HERMES_DEBUG_LLM")) {
                std::fprintf(stderr, "[hermes-llm] %s status=%d body=%.1000s\n",
                             base_url_.c_str(), resp.status_code,
                             resp.body.c_str());
            }
            if (resp.status_code < 200 || resp.status_code >= 300) {
                throw ApiError(resp.status_code, resp.body, provider_name());
            }
            json parsed;
            try {
                parsed = json::parse(resp.body);
            } catch (const std::exception& e) {
                throw ApiError(
                    resp.status_code,
                    std::string("invalid JSON response: ") + e.what(),
                    provider_name());
            }
            last_response =
                parse_chat_completions_response(parsed, provider_name());
        }

        if (!is_codex_intermediate_ack(last_response, provider_name(),
                                       base_url_)) {
            return last_response;
        }
        // else: loop and re-issue the request.  After kMaxAckRetries we
        // return the last ack as-is so callers see it (matching Python's
        // behaviour of surrendering after the retry budget is exhausted).
    }
    return last_response;
}

}  // namespace hermes::llm
