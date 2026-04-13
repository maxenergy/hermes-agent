// QwenClient — LlmClient implementation backed by Qwen Code OAuth credentials.
//
// The Qwen Plus / Qwen3.6-Plus models are exposed as an OpenAI-compatible
// `/v1/chat/completions` endpoint.  This class wraps OpenAIClient with
// transparent token refresh: every complete() call grabs the latest cached
// token (refreshing if it's about to expire) before delegating.
#pragma once

#include "hermes/auth/qwen_oauth.hpp"
#include "hermes/llm/llm_client.hpp"

#include <memory>
#include <mutex>
#include <string>

namespace hermes::auth {

class QwenClient : public hermes::llm::LlmClient {
public:
    explicit QwenClient(hermes::llm::HttpTransport* transport = nullptr,
                        QwenCredentialStore store = QwenCredentialStore());

    hermes::llm::CompletionResponse complete(
        const hermes::llm::CompletionRequest& req) override;

    std::string provider_name() const override { return "qwen"; }

    // Convenience: returns true if a non-empty credential blob exists on disk.
    bool is_authenticated() const;

    // Resolve the current API base (e.g. "https://portal.qwen.ai/v1").
    std::string current_base_url();

private:
    hermes::llm::HttpTransport* transport_;
    QwenCredentialStore store_;
    QwenOAuth oauth_;
    std::mutex mu_;

    QwenCredentials get_fresh_credentials();
};

}  // namespace hermes::auth
