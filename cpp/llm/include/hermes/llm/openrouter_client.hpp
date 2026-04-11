// OpenRouter client — same wire format as OpenAI chat-completions but adds
// HTTP-Referer and X-Title headers so the provider can attribute usage.
#pragma once

#include "hermes/llm/llm_client.hpp"

#include <string>

namespace hermes::llm {

class OpenRouterClient : public LlmClient {
public:
    OpenRouterClient(HttpTransport* transport,
                     std::string api_key,
                     std::string referer = "https://github.com/NousResearch/hermes-agent",
                     std::string title = "hermes-agent");

    CompletionResponse complete(const CompletionRequest& req) override;
    std::string provider_name() const override { return "openrouter"; }

private:
    HttpTransport* transport_;
    std::string api_key_;
    std::string referer_;
    std::string title_;
};

}  // namespace hermes::llm
