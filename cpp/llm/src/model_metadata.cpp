// Model metadata lookup, token estimation, and context-limit parsing.
#include "hermes/llm/model_metadata.hpp"

#include "hermes/llm/message.hpp"
#include "hermes/llm/usage.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <regex>
#include <string>

namespace hermes::llm {

namespace {

struct KnownModel {
    const char* key;
    const char* family;
    int64_t context_length;
    bool supports_reasoning;
    bool supports_vision;
    bool supports_prompt_cache;
};

// Broad family fallbacks.  Specific ids resolve first by prefix match;
// callers may overlay models.dev data in Phase 4.
constexpr std::array<KnownModel, 12> kKnownModels = {{
    {"claude-opus-4-6",   "claude", 1'000'000, true,  true,  true},
    {"claude-sonnet-4-6", "claude", 1'000'000, true,  true,  true},
    {"claude-haiku-4-5",  "claude",   200'000, false, true,  true},
    {"claude-opus",       "claude",   200'000, true,  true,  true},
    {"claude-sonnet",     "claude",   200'000, false, true,  true},
    {"claude-haiku",      "claude",   200'000, false, true,  true},
    {"gpt-4o-mini",       "gpt",      128'000, false, true,  false},
    {"gpt-4o",            "gpt",      128'000, false, true,  false},
    {"gpt-4.1",           "gpt",    1'047'576, false, true,  false},
    {"gpt-5",             "gpt",      128'000, true,  true,  false},
    {"deepseek-chat",     "deepseek",  64'000, false, false, false},
    {"gemini-2.0-flash",  "gemini", 1'000'000, false, true,  false},
}};

bool starts_with_ci(std::string_view hay, std::string_view needle) {
    if (hay.size() < needle.size()) return false;
    for (size_t i = 0; i < needle.size(); ++i) {
        const char a = static_cast<char>(
            std::tolower(static_cast<unsigned char>(hay[i])));
        const char b = static_cast<char>(
            std::tolower(static_cast<unsigned char>(needle[i])));
        if (a != b) return false;
    }
    return true;
}

bool contains_ci(std::string_view hay, std::string_view needle) {
    if (needle.empty()) return true;
    if (hay.size() < needle.size()) return false;
    for (size_t i = 0; i + needle.size() <= hay.size(); ++i) {
        bool ok = true;
        for (size_t j = 0; j < needle.size(); ++j) {
            const char a = static_cast<char>(
                std::tolower(static_cast<unsigned char>(hay[i + j])));
            const char b = static_cast<char>(
                std::tolower(static_cast<unsigned char>(needle[j])));
            if (a != b) {
                ok = false;
                break;
            }
        }
        if (ok) return true;
    }
    return false;
}

}  // namespace

std::string strip_provider_prefix(std::string_view model) {
    // Provider prefixes use "/" (e.g. anthropic/claude-opus-4-6).  The
    // Python version also handles "prefix:" but avoids stripping Ollama
    // "model:tag" forms; we replicate only the "/" case here since
    // ":" prefixes are uncommon in the Phase 3 test surface.
    auto slash = model.find('/');
    if (slash == std::string_view::npos) {
        return std::string(model);
    }
    return std::string(model.substr(slash + 1));
}

ModelMetadata fetch_model_metadata(std::string_view model,
                                   std::string_view /*base_url*/) {
    const std::string stripped = strip_provider_prefix(model);
    ModelMetadata md;
    md.model_id = stripped;
    md.source = "hardcoded";
    md.context_length = -1;

    for (const auto& km : kKnownModels) {
        if (starts_with_ci(stripped, km.key) || contains_ci(stripped, km.key)) {
            md.family = km.family;
            md.context_length = km.context_length;
            md.supports_reasoning = km.supports_reasoning;
            md.supports_vision = km.supports_vision;
            md.supports_prompt_cache = km.supports_prompt_cache;
            break;
        }
    }
    md.pricing = lookup_pricing(stripped);
    return md;
}

int64_t estimate_tokens_rough(std::string_view text) {
    if (text.empty()) return 0;
    return static_cast<int64_t>(text.size()) / 4;
}

int64_t estimate_messages_tokens_rough(const std::vector<Message>& messages) {
    int64_t total = 0;
    for (const auto& m : messages) {
        total += estimate_tokens_rough(m.content_text);
        for (const auto& b : m.content_blocks) {
            total += estimate_tokens_rough(b.text);
            if (!b.extra.is_null()) {
                total += estimate_tokens_rough(b.extra.dump());
            }
        }
        for (const auto& tc : m.tool_calls) {
            total += estimate_tokens_rough(tc.name);
            if (!tc.arguments.is_null()) {
                total += estimate_tokens_rough(tc.arguments.dump());
            }
        }
        if (m.reasoning) {
            total += estimate_tokens_rough(*m.reasoning);
        }
    }
    return total;
}

std::optional<int64_t> parse_context_limit_from_error(std::string_view error_body) {
    // Lowercase once for the regexes.
    std::string lower;
    lower.reserve(error_body.size());
    for (char c : error_body) {
        lower.push_back(static_cast<char>(
            std::tolower(static_cast<unsigned char>(c))));
    }

    // Patterns mirror agent/model_metadata.py::parse_context_limit_from_error.
    static const std::array<std::regex, 5> patterns = {
        std::regex(R"((?:max(?:imum)?|limit)\s*(?:context\s*)?(?:length|size|window)?\s*(?:is|of|:)?\s*(\d{4,}))"),
        std::regex(R"(context\s*(?:length|size|window)\s*(?:is|of|:)?\s*(\d{4,}))"),
        std::regex(R"((\d{4,})\s*(?:token)?\s*(?:context|limit))"),
        std::regex(R"(>\s*(\d{4,})\s*(?:max|limit|token))"),
        std::regex(R"((\d{4,})\s*(?:max(?:imum)?)\b)"),
    };

    for (const auto& pat : patterns) {
        std::smatch m;
        if (std::regex_search(lower, m, pat)) {
            try {
                const int64_t value = std::stoll(m[1].str());
                if (value >= 1024 && value <= 10'000'000) {
                    return value;
                }
            } catch (const std::exception&) {
                // fall through
            }
        }
    }
    return std::nullopt;
}

}  // namespace hermes::llm
