// OpenAI chat-completions client (non-streaming).
#pragma once

#include "hermes/llm/llm_client.hpp"

#include <string>

namespace hermes::llm {

class OpenAIClient : public LlmClient {
public:
    OpenAIClient(HttpTransport* transport,
                 std::string api_key,
                 std::string base_url = "https://api.openai.com/v1");

    CompletionResponse complete(const CompletionRequest& req) override;
    std::string provider_name() const override { return "openai"; }

private:
    HttpTransport* transport_;
    std::string api_key_;
    std::string base_url_;
};

}  // namespace hermes::llm
