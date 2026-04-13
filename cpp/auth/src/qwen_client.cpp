#include "hermes/auth/qwen_client.hpp"

#include <stdexcept>

#include "hermes/llm/openai_client.hpp"

namespace hermes::auth {

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

    // Qwen serves the OpenAI chat-completions schema verbatim.  Build a
    // single-shot OpenAIClient pointing at the resolved endpoint with the
    // freshly-refreshed access token.
    hermes::llm::OpenAIClient inner(transport_, creds.access_token, base_url);
    return inner.complete(req);
}

}  // namespace hermes::auth
