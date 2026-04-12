#include "hermes/agent/auxiliary_client.hpp"

#include "hermes/llm/anthropic_client.hpp"
#include "hermes/llm/openai_client.hpp"
#include "hermes/llm/openrouter_client.hpp"

#include <stdexcept>

namespace hermes::agent {

namespace {

class StubClient : public hermes::llm::LlmClient {
public:
    explicit StubClient(std::string reason) : reason_(std::move(reason)) {}
    hermes::llm::CompletionResponse complete(
        const hermes::llm::CompletionRequest&) override {
        throw std::runtime_error("auxiliary LLM client not configured: " +
                                 reason_);
    }
    std::string provider_name() const override { return "stub"; }

private:
    std::string reason_;
};

const nlohmann::json* find_aux(const nlohmann::json& cfg) {
    if (!cfg.is_object()) return nullptr;
    if (cfg.contains("auxiliary") && cfg["auxiliary"].is_object()) {
        return &cfg["auxiliary"];
    }
    return nullptr;
}

std::string str_or_empty(const nlohmann::json& obj, const char* key) {
    if (!obj.is_object() || !obj.contains(key)) return {};
    if (!obj[key].is_string()) return {};
    return obj[key].get<std::string>();
}

}  // namespace

std::unique_ptr<hermes::llm::LlmClient> make_auxiliary_client(
    const nlohmann::json& config,
    hermes::llm::HttpTransport* transport) {
    const auto* aux = find_aux(config);
    if (!aux) {
        return std::make_unique<StubClient>("missing 'auxiliary' block");
    }
    const std::string provider = str_or_empty(*aux, "provider");
    const std::string api_key = str_or_empty(*aux, "api_key");
    const std::string base_url = str_or_empty(*aux, "base_url");
    if (provider.empty()) {
        return std::make_unique<StubClient>("missing 'auxiliary.provider'");
    }
    if (!transport) {
        return std::make_unique<StubClient>("no HttpTransport supplied");
    }

    if (provider == "openai") {
        return std::make_unique<hermes::llm::OpenAIClient>(
            transport, api_key,
            base_url.empty() ? "https://api.openai.com/v1" : base_url);
    }
    if (provider == "anthropic") {
        return std::make_unique<hermes::llm::AnthropicClient>(
            transport, api_key,
            base_url.empty() ? "https://api.anthropic.com/v1" : base_url);
    }
    if (provider == "openrouter") {
        return std::make_unique<hermes::llm::OpenRouterClient>(
            transport, api_key,
            "https://github.com/NousResearch/hermes-agent", "hermes-agent");
    }
    return std::make_unique<StubClient>("unknown provider: " + provider);
}

std::string get_auxiliary_model(const nlohmann::json& config) {
    const auto* aux = find_aux(config);
    if (!aux) return {};
    return str_or_empty(*aux, "model");
}

}  // namespace hermes::agent
