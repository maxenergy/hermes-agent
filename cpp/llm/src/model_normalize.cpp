#include "hermes/llm/model_normalize.hpp"

#include <algorithm>
#include <cctype>

namespace hermes::llm {

std::string normalize_model_id(const std::string& model) {
    // Strip all provider prefixes delimited by '/'.
    // "openrouter/anthropic/claude-opus-4-6" -> "claude-opus-4-6"
    auto last_slash = model.rfind('/');
    if (last_slash == std::string::npos) {
        return model;
    }
    return model.substr(last_slash + 1);
}

bool is_codex_model(const std::string& model) {
    std::string lower = normalize_model_id(model);
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    // Codex / code family.
    if (lower.rfind("codex-", 0) == 0) return true;
    if (lower.rfind("code-", 0) == 0) return true;

    // GPT-4o and GPT-4.1 family are code-capable.
    if (lower.rfind("gpt-4o", 0) == 0) return true;
    if (lower.rfind("gpt-4.1", 0) == 0) return true;
    if (lower.rfind("gpt-5", 0) == 0) return true;

    // Claude family.
    if (lower.rfind("claude-", 0) == 0) return true;

    // DeepSeek Coder.
    if (lower.find("deepseek-coder") != std::string::npos) return true;

    // Qwen Coder.
    if (lower.find("qwen") != std::string::npos &&
        lower.find("coder") != std::string::npos)
        return true;

    return false;
}

}  // namespace hermes::llm
