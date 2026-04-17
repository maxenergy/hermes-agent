// OpenAI chat-completions client (non-streaming).
#pragma once

#include "hermes/llm/llm_client.hpp"

#include <string>
#include <unordered_map>
#include <utility>

namespace hermes::llm {

class OpenAIClient : public LlmClient {
public:
    OpenAIClient(HttpTransport* transport,
                 std::string api_key,
                 std::string base_url = "https://api.openai.com/v1");

    CompletionResponse complete(const CompletionRequest& req) override;
    std::string provider_name() const override {
        return provider_name_.empty() ? "openai" : provider_name_;
    }

    // Extra HTTP headers sent on every request (used by Qwen for the
    // X-DashScope-* family).  Caller-set after construction.
    void set_extra_headers(std::unordered_map<std::string, std::string> h) {
        extra_headers_ = std::move(h);
    }
    void set_provider_name(std::string name) { provider_name_ = std::move(name); }
    // Force streaming regardless of ``CompletionRequest::stream``.  Some
    // backends (e.g. the Codex/ChatGPT CLIProxy) return ``content:null``
    // under non-stream mode and only populate content via the SSE
    // delta, so the adapter that constructs them should opt in.
    void set_force_stream(bool f) { force_stream_ = f; }

private:
    HttpTransport* transport_;
    std::string api_key_;
    std::string base_url_;
    std::unordered_map<std::string, std::string> extra_headers_;
    std::string provider_name_;
    bool force_stream_ = false;
};

}  // namespace hermes::llm
