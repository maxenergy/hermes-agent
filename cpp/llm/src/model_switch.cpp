#include "hermes/llm/model_switch.hpp"

#include "hermes/llm/codex_models.hpp"
#include "hermes/llm/model_normalize.hpp"

#include <algorithm>
#include <cctype>
#include <mutex>
#include <stdexcept>

namespace hermes::llm {

namespace {

std::mutex& factory_mu() {
    static std::mutex m;
    return m;
}
LlmClientFactory& factory_ref() {
    static LlmClientFactory f;
    return f;
}

std::string to_lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

// Tokenizer families.  Providers within the same family share tokenizers
// (approximately) so switching between them doesn't require a token
// recount.  Anything unknown is its own family.
std::string tokenizer_family(const std::string& provider) {
    if (provider == "anthropic") return "anthropic";
    if (provider == "openai" || provider == "openai-codex" ||
        provider == "openrouter" || provider == "copilot") {
        return "openai";
    }
    if (provider == "qwen-oauth" || provider == "deepseek") return "qwen";
    if (provider == "google") return "google";
    return provider;
}

}  // namespace

void set_llm_client_factory(LlmClientFactory factory) {
    std::lock_guard<std::mutex> lock(factory_mu());
    factory_ref() = std::move(factory);
}

LlmClientFactory get_llm_client_factory() {
    std::lock_guard<std::mutex> lock(factory_mu());
    return factory_ref();
}

std::vector<std::string> build_tier_down(const std::string& model) {
    const std::string normalized = normalize_model_id(model);
    const std::string lower = to_lower(normalized);

    std::vector<std::string> out;
    auto push = [&](const char* m) {
        if (std::find(out.begin(), out.end(), m) == out.end() &&
            to_lower(m) != lower) {
            out.emplace_back(m);
        }
    };

    // Anthropic family — Opus → Sonnet → Haiku.
    if (lower.rfind("claude-opus", 0) == 0) {
        push("claude-sonnet-4-6");
        push("claude-haiku-4-6");
    } else if (lower.rfind("claude-sonnet", 0) == 0) {
        push("claude-haiku-4-6");
    }

    // OpenAI family — big model → mini → nano.
    if (lower.rfind("gpt-5", 0) == 0) {
        if (lower.find("codex") != std::string::npos) {
            push("gpt-5.1-codex-mini");
        } else if (lower.find("mini") == std::string::npos) {
            push("gpt-5.4-mini");
        }
    } else if (lower.rfind("gpt-4o", 0) == 0 &&
               lower.find("mini") == std::string::npos) {
        push("gpt-4o-mini");
    }

    // Qwen family — Max → Plus → Turbo.
    if (lower.rfind("qwen3-max", 0) == 0 ||
        lower.find("qwen-max") != std::string::npos) {
        push("qwen3-coder-plus");
        push("qwen-turbo");
    } else if (lower.rfind("qwen3-coder", 0) == 0 ||
               lower.find("qwen-coder") != std::string::npos) {
        push("qwen-turbo");
    }

    return out;
}

ModelSwitchResult switch_model(
    const std::string& new_model,
    const nlohmann::json& config,
    const std::string& previous_provider,
    CredentialPool* pool) {
    if (new_model.empty()) {
        throw std::invalid_argument("switch_model: new_model must be non-empty");
    }

    ModelSwitchResult result;
    result.resolved = resolve_runtime_provider(new_model, config, pool,
                                               /*allow_missing_key=*/true);

    const std::string old_family = tokenizer_family(previous_provider);
    const std::string new_family = tokenizer_family(result.resolved.provider_name);
    result.tokenizer_invalidated =
        !previous_provider.empty() && old_family != new_family;

    result.tier_down = build_tier_down(new_model);

    // Build a new client via the registered factory.  If none is
    // registered, leave `client` null — the caller (test) can inspect
    // `resolved` directly.  In production cpp/agent wires a real factory.
    LlmClientFactory factory;
    {
        std::lock_guard<std::mutex> lock(factory_mu());
        factory = factory_ref();
    }
    if (factory) {
        result.client = factory(result.resolved);
    }

    result.summary = "model=" + result.resolved.model_id +
                     " provider=" + result.resolved.provider_name +
                     " base_url=" + result.resolved.base_url +
                     " api_mode=" + result.resolved.api_mode +
                     " source=" + result.resolved.source;
    return result;
}

}  // namespace hermes::llm
