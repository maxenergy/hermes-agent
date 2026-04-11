// Anthropic Messages API client (non-streaming).  Applies the
// system_and_3 prompt-cache strategy when cache.native_anthropic is true.
#pragma once

#include "hermes/llm/llm_client.hpp"

#include <string>

namespace hermes::llm {

class AnthropicClient : public LlmClient {
public:
    AnthropicClient(HttpTransport* transport,
                    std::string api_key,
                    std::string base_url = "https://api.anthropic.com/v1");

    CompletionResponse complete(const CompletionRequest& req) override;
    std::string provider_name() const override { return "anthropic"; }

private:
    HttpTransport* transport_;
    std::string api_key_;
    std::string base_url_;
};

}  // namespace hermes::llm
