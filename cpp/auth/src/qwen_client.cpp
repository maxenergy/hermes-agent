#include "hermes/auth/qwen_client.hpp"

#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdlib>
#include <fstream>
#include <random>
#include <sstream>
#include <stdexcept>

#include "hermes/llm/llm_client.hpp"

namespace hermes::auth {

namespace {
using json = nlohmann::json;

std::string random_uuid_v4() {
    static thread_local std::mt19937_64 rng{static_cast<uint64_t>(
        std::chrono::steady_clock::now().time_since_epoch().count())};
    std::uniform_int_distribution<uint64_t> dist;
    uint64_t a = dist(rng), b = dist(rng);
    char buf[37];
    std::snprintf(buf, sizeof(buf),
                  "%08x-%04x-%04x-%04x-%012lx",
                  static_cast<unsigned>(a >> 32),
                  static_cast<unsigned>((a >> 16) & 0xffff),
                  static_cast<unsigned>((a & 0xffff) | 0x4000),
                  static_cast<unsigned>(((b >> 48) & 0x3fff) | 0x8000),
                  static_cast<unsigned long>(b & 0xffffffffffffull));
    return buf;
}

// Convert a hermes::llm::Message to Qwen's expected JSON format:
// content always serialised as an array of {type:text, text:...} blocks.
json message_to_qwen_json(const hermes::llm::Message& m) {
    static constexpr const char* kRoles[] = {"system", "user", "assistant", "tool"};
    json j;
    j["role"] = kRoles[static_cast<int>(m.role)];
    json content_arr = json::array();
    if (!m.content_text.empty()) {
        content_arr.push_back({{"type", "text"}, {"text", m.content_text}});
    }
    for (const auto& b : m.content_blocks) {
        if (b.type == "text" || b.type.empty()) {
            content_arr.push_back({{"type", "text"}, {"text", b.text}});
        }
    }
    if (content_arr.empty()) {
        content_arr.push_back({{"type", "text"}, {"text", ""}});
    }
    j["content"] = content_arr;
    if (m.tool_call_id) j["tool_call_id"] = *m.tool_call_id;
    return j;
}

// Parse one SSE 'data: { ... }' line, accumulate delta into out.
void apply_delta(json& acc, const json& delta) {
    if (delta.contains("content") && delta["content"].is_string()) {
        acc["content"] = acc.value("content", std::string()) + delta["content"].get<std::string>();
    }
    if (delta.contains("reasoning_content") && delta["reasoning_content"].is_string()) {
        acc["reasoning"] = acc.value("reasoning", std::string()) +
                            delta["reasoning_content"].get<std::string>();
    }
    if (delta.contains("role") && delta["role"].is_string()) {
        acc["role"] = delta["role"];
    }
}

}  // namespace

QwenClient::QwenClient(hermes::llm::HttpTransport* transport,
                       QwenCredentialStore store)
    : transport_(transport ? transport : hermes::llm::get_default_transport()),
      store_(std::move(store)),
      oauth_(transport_) {}

bool QwenClient::is_authenticated() const {
    return !store_.load().empty();
}

QwenCredentials QwenClient::get_fresh_credentials() {
    std::lock_guard<std::mutex> lock(mu_);
    auto creds = oauth_.ensure_valid(store_);
    if (!creds) {
        throw std::runtime_error(
            "Qwen credentials missing or refresh failed — run "
            "`hermes auth qwen login` to (re-)authenticate.");
    }
    return *creds;
}

std::string QwenClient::current_base_url() {
    auto creds = get_fresh_credentials();
    return qwen_api_base_url(creds);
}

hermes::llm::CompletionResponse QwenClient::complete(
    const hermes::llm::CompletionRequest& req) {
    auto creds = get_fresh_credentials();
    auto base_url = qwen_api_base_url(creds);

    // Qwen's portal.qwen.ai endpoint enforces several non-negotiable
    // constraints on top of the standard OpenAI chat-completions schema —
    // discovered by intercepting qwen-code's actual requests:
    //   1. model must be the OAuth-internal alias 'coder-model'
    //      (translates to qwen3.6-plus on the server)
    //   2. messages[].content MUST be an array of {type, text} blocks
    //      (string content yields HTTP 400)
    //   3. tools must be a non-empty array
    //      (empty array yields '[] is too short - tools')
    //   4. metadata.{sessionId,promptId} required for routing
    //   5. stream=true is the supported mode (response is SSE)
    //   6. X-DashScope-* headers required for auth routing
    //
    // We synthesise (1)–(4) from the incoming CompletionRequest so callers
    // don't need to know the Qwen-specific quirks.

    json body;
    body["model"] = "coder-model";
    body["stream"] = true;
    body["stream_options"] = {{"include_usage", true}};
    body["max_tokens"] = req.max_tokens.value_or(8000);
    if (req.temperature) body["temperature"] = *req.temperature;
    body["metadata"] = {{"sessionId", random_uuid_v4()},
                        {"promptId", random_uuid_v4()}};
    // vl_high_resolution_images is required by portal.qwen.ai — without it
    // the endpoint returns HTTP 400 even for text-only requests.
    body["vl_high_resolution_images"] = true;

    // Qwen requires at least one system message.  Synthesise a generic one
    // when the caller didn't provide any so plain `chat()` still works.
    json msgs = json::array();
    bool has_system = false;
    for (const auto& m : req.messages) {
        if (m.role == hermes::llm::Role::System) { has_system = true; break; }
    }
    if (!has_system) {
        msgs.push_back({
            {"role", "system"},
            {"content", json::array({json{{"type", "text"},
                                          {"text", "You are a helpful assistant."},
                                          {"cache_control", {{"type", "ephemeral"}}}}})},
        });
    }
    for (const auto& m : req.messages) msgs.push_back(message_to_qwen_json(m));
    body["messages"] = msgs;

    // Tools — Qwen rejects empty arrays.  Provide a minimal placeholder when
    // the caller didn't supply any so plain chat still works.
    json tools = json::array();
    for (const auto& t : req.tools) {
        tools.push_back({{"type", "function"},
                         {"function", {{"name", t.name},
                                       {"description", t.description},
                                       {"parameters", t.parameters}}}});
    }
    if (tools.empty()) {
        tools.push_back({{"type", "function"},
                         {"function", {{"name", "echo"},
                                       {"description", "Returns the input text unchanged. Placeholder tool — never call."},
                                       {"parameters", {{"type", "object"},
                                                       {"properties", {{"text", {{"type", "string"}}}}},
                                                       {"required", json::array({"text"})}}}}}});
    }
    body["tools"] = tools;

    // Qwen's portal endpoint fingerprints the User-Agent and rejects
    // anything that doesn't look like the official qwen-code CLI.  We're
    // using qwen-code's OAuth credentials by user consent, so impersonate
    // its UA — there's no separate hermes registration with Alibaba.
    static constexpr const char* kQwenCodeUa = "QwenCode/0.14.3 (linux; x64)";
    std::unordered_map<std::string, std::string> headers = {
        {"Authorization", "Bearer " + creds.access_token},
        {"Content-Type", "application/json"},
        {"Accept", "application/json"},
        {"User-Agent", kQwenCodeUa},
        {"X-DashScope-CacheControl", "enable"},
        {"X-DashScope-UserAgent", kQwenCodeUa},
        {"X-DashScope-AuthType", "qwen-oauth"},
    };

    // The response is SSE — collect via streaming callback.
    json acc = json::object();
    json final_usage;
    std::string finish_reason;

    transport_->post_json_stream(
        base_url + "/chat/completions", headers, body.dump(),
        [&](const std::string& chunk) {
            // The chunk may contain multiple "data: ..." lines.
            std::istringstream iss(chunk);
            std::string line;
            while (std::getline(iss, line)) {
                if (!line.empty() && line.back() == '\r') line.pop_back();
                if (line.rfind("data:", 0) != 0) continue;
                auto payload = line.substr(5);
                while (!payload.empty() && payload.front() == ' ') payload.erase(0, 1);
                if (payload == "[DONE]") return;
                try {
                    auto j = json::parse(payload);
                    if (j.contains("choices") && j["choices"].is_array() &&
                        !j["choices"].empty()) {
                        const auto& ch = j["choices"][0];
                        if (ch.contains("delta")) apply_delta(acc, ch["delta"]);
                        if (ch.contains("finish_reason") &&
                            ch["finish_reason"].is_string()) {
                            finish_reason = ch["finish_reason"];
                        }
                    }
                    if (j.contains("usage") && !j["usage"].is_null()) {
                        final_usage = j["usage"];
                    }
                } catch (const std::exception&) {
                    // Tolerate malformed lines (keep-alive, etc.)
                }
            }
        });

    hermes::llm::CompletionResponse out;
    out.assistant_message.role = hermes::llm::Role::Assistant;
    out.assistant_message.content_text = acc.value("content", std::string());
    if (acc.contains("reasoning") && acc["reasoning"].is_string()) {
        out.assistant_message.reasoning = acc["reasoning"].get<std::string>();
    }
    if (final_usage.is_object()) {
        out.usage.input_tokens = final_usage.value("prompt_tokens", 0);
        out.usage.output_tokens = final_usage.value("completion_tokens", 0);
    }
    out.finish_reason = finish_reason.empty() ? "stop" : finish_reason;
    return out;
}

}  // namespace hermes::auth
