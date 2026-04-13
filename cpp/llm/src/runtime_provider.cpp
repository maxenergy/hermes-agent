#include "hermes/llm/runtime_provider.hpp"

#include "hermes/llm/codex_models.hpp"
#include "hermes/llm/model_normalize.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <stdexcept>
#include <string>

namespace hermes::llm {

namespace {

std::string to_lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

std::string rstrip_slash(std::string s) {
    while (!s.empty() && s.back() == '/') s.pop_back();
    return s;
}

std::string get_env(const char* name) {
    const char* v = std::getenv(name);
    return v ? std::string(v) : std::string();
}

std::string json_str(const nlohmann::json& j, const char* key) {
    if (!j.is_object()) return "";
    auto it = j.find(key);
    if (it == j.end() || !it->is_string()) return "";
    std::string s = it->get<std::string>();
    // trim
    std::size_t a = s.find_first_not_of(" \t\r\n");
    std::size_t b = s.find_last_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    return s.substr(a, b - a + 1);
}

// Look up `config["model"]` as a JSON object (accepting both the
// dict-shaped and the legacy string-shaped forms).
nlohmann::json extract_model_cfg(const nlohmann::json& config) {
    if (!config.is_object()) return nlohmann::json::object();
    auto it = config.find("model");
    if (it == config.end()) return nlohmann::json::object();
    if (it->is_object()) return *it;
    if (it->is_string()) {
        nlohmann::json obj = nlohmann::json::object();
        obj["default"] = it->get<std::string>();
        return obj;
    }
    return nlohmann::json::object();
}

}  // namespace

std::string infer_provider_from_model(const std::string& model) {
    if (model.empty()) return "auto";

    // Leading-namespace prefix wins:  "openrouter/foo" → openrouter.
    auto first_slash = model.find('/');
    if (first_slash != std::string::npos) {
        std::string prefix = to_lower(model.substr(0, first_slash));
        if (prefix == "openrouter") return "openrouter";
        if (prefix == "anthropic") return "anthropic";
        if (prefix == "openai") {
            // openai/gpt-5.3-codex → openai-codex if slug matches
            std::string tail = model.substr(first_slash + 1);
            if (is_codex_backed_model(tail)) return "openai-codex";
            return "openai";
        }
        if (prefix == "qwen") return "qwen-oauth";
        if (prefix == "google") return "google";
        if (prefix == "deepseek") return "deepseek";
        if (prefix == "copilot" || prefix == "github-copilot") return "copilot";
        if (prefix == "nous" || prefix == "nousresearch") return "nous";
    }

    const std::string normalized = normalize_model_id(model);
    const std::string lower = to_lower(normalized);

    // Codex-backed first (narrower than is_codex_model() heuristic).
    if (is_codex_backed_model(normalized)) return "openai-codex";

    if (lower.rfind("claude", 0) == 0) return "anthropic";
    if (lower.rfind("gpt-", 0) == 0 || lower.rfind("o3", 0) == 0 ||
        lower.rfind("o4", 0) == 0 || lower == "chatgpt" ||
        lower.rfind("gpt4", 0) == 0) {
        return "openai";
    }
    if (lower.rfind("qwen", 0) == 0) return "qwen-oauth";
    if (lower.rfind("gemini", 0) == 0) return "google";
    if (lower.rfind("deepseek", 0) == 0) return "deepseek";
    if (lower.find("hermes") != std::string::npos) return "nous";
    if (lower.rfind("grok", 0) == 0) return "x-ai";

    return "openrouter";  // catch-all aggregator
}

std::string default_base_url_for_provider(const std::string& provider) {
    if (provider == "anthropic") return "https://api.anthropic.com";
    if (provider == "openai") return "https://api.openai.com/v1";
    if (provider == "openai-codex") return "https://chatgpt.com/backend-api/codex";
    if (provider == "qwen-oauth") return "https://dashscope.aliyuncs.com/compatible-mode/v1";
    if (provider == "openrouter") return "https://openrouter.ai/api/v1";
    if (provider == "copilot") return "https://api.githubcopilot.com";
    if (provider == "nous") return "https://inference-api.nousresearch.com/v1";
    if (provider == "google") return "https://generativelanguage.googleapis.com/v1beta";
    if (provider == "deepseek") return "https://api.deepseek.com/v1";
    if (provider == "x-ai") return "https://api.x.ai/v1";
    return "";
}

std::string default_api_mode_for_provider(const std::string& provider) {
    if (provider == "anthropic") return "anthropic_messages";
    if (provider == "openai-codex") return "codex_responses";
    return "chat_completions";
}

ResolvedProvider resolve_runtime_provider(
    const std::string& model,
    const nlohmann::json& config,
    CredentialPool* pool,
    bool allow_missing_key) {
    const nlohmann::json model_cfg = extract_model_cfg(config);

    // 1. Decide provider.
    std::string provider = to_lower(json_str(model_cfg, "provider"));
    if (provider.empty() || provider == "auto") {
        provider = infer_provider_from_model(model);
    }

    // 2. Resolve base URL.
    std::string base_url = rstrip_slash(json_str(model_cfg, "base_url"));
    std::string api_key = json_str(model_cfg, "api_key");
    std::string source;

    if (!api_key.empty()) {
        source = "config";
    }

    // 3. Consult credential pool (if provided) when config didn't supply.
    CredentialPool* effective_pool =
        pool ? pool : &CredentialPool::global();
    if (api_key.empty() || base_url.empty()) {
        auto cached = effective_pool->get(provider);
        if (cached.has_value()) {
            if (api_key.empty()) {
                api_key = cached->api_key;
                source = cached->source.empty() ? std::string("pool") : cached->source;
            }
            if (base_url.empty()) {
                base_url = rstrip_slash(cached->base_url);
            }
        }
    }

    // 4. Fall back to env var for api_key.
    if (api_key.empty()) {
        // Provider-specific env var first, then generic fallbacks.
        auto upper = [](std::string s) {
            std::transform(s.begin(), s.end(), s.begin(),
                           [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
            return s;
        };
        std::string env_name = upper(provider);
        // hyphens → underscores for env-var friendliness
        std::replace(env_name.begin(), env_name.end(), '-', '_');
        std::string cand = get_env((env_name + "_API_KEY").c_str());
        if (cand.empty()) {
            cand = get_env(("HERMES_" + env_name + "_API_KEY").c_str());
        }
        if (cand.empty() && (provider == "openai" || provider == "openai-codex")) {
            cand = get_env("OPENAI_API_KEY");
        }
        if (cand.empty() && provider == "anthropic") {
            cand = get_env("ANTHROPIC_API_KEY");
            if (cand.empty()) cand = get_env("ANTHROPIC_TOKEN");
        }
        if (cand.empty() && provider == "openrouter") {
            cand = get_env("OPENROUTER_API_KEY");
        }
        if (!cand.empty()) {
            api_key = cand;
            source = "env";
        }
    }

    // 5. Default base URL.
    if (base_url.empty()) {
        base_url = default_base_url_for_provider(provider);
        if (source.empty()) source = "default";
    }

    // 6. API mode — honour explicit config value, else default per provider.
    std::string api_mode = to_lower(json_str(model_cfg, "api_mode"));
    if (api_mode != "chat_completions" && api_mode != "anthropic_messages" &&
        api_mode != "codex_responses") {
        api_mode = default_api_mode_for_provider(provider);
    }

    // 7. Validate.
    if (api_key.empty() && !allow_missing_key) {
        // For local endpoints (ollama-style), an empty key is OK — use a
        // placeholder.  We heuristically detect localhost-style base URLs.
        const std::string b_lower = to_lower(base_url);
        if (b_lower.find("localhost") != std::string::npos ||
            b_lower.find("127.0.0.1") != std::string::npos ||
            b_lower.find("0.0.0.0") != std::string::npos) {
            api_key = "no-key-required";
            source = "local";
        } else {
            throw std::runtime_error(
                "resolve_runtime_provider: no credentials found for provider '" +
                provider + "' (model=" + model + ")");
        }
    }

    ResolvedProvider r;
    r.provider_name = provider;
    r.api_key = api_key;
    r.base_url = base_url;
    r.model_id = normalize_model_id(model);
    r.api_mode = api_mode;
    r.source = source.empty() ? std::string("default") : source;
    return r;
}

}  // namespace hermes::llm
