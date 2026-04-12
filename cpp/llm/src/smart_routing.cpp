#include "hermes/llm/smart_routing.hpp"

#include "hermes/llm/model_metadata.hpp"
#include "hermes/llm/model_normalize.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <string>
#include <utility>
#include <vector>

namespace hermes::llm {

namespace {

// Context-ordered model tiers: models with progressively larger context.
struct ContextTier {
    const char* model;
    int64_t context_length;
};

constexpr std::array<ContextTier, 7> kContextTiers = {{
    {"gpt-4o-mini",       128'000},
    {"gpt-4o",            128'000},
    {"claude-haiku",      200'000},
    {"claude-sonnet",     200'000},
    {"gpt-4.1",         1'047'576},
    {"claude-opus-4-6", 1'000'000},
    {"gemini-2.0-flash",1'000'000},
}};

// Fallback chains for common failure modes.
struct FallbackEntry {
    const char* from;
    FailoverReason reason;
    const char* to;
};

constexpr std::array<FallbackEntry, 8> kFallbacks = {{
    {"claude-opus-4-6",   FailoverReason::RateLimit,        "claude-sonnet"},
    {"claude-sonnet",     FailoverReason::RateLimit,        "claude-haiku"},
    {"gpt-4o",            FailoverReason::RateLimit,        "gpt-4o-mini"},
    {"gpt-5",             FailoverReason::RateLimit,        "gpt-4o"},
    {"claude-opus-4-6",   FailoverReason::ModelUnavailable, "claude-sonnet"},
    {"claude-sonnet",     FailoverReason::ModelUnavailable, "claude-haiku"},
    {"gpt-4o",            FailoverReason::ModelUnavailable, "gpt-4o-mini"},
    {"gpt-5",             FailoverReason::ModelUnavailable, "gpt-4o"},
}};

std::string to_lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

bool starts_with(const std::string& hay, const char* needle) {
    std::string n(needle);
    if (hay.size() < n.size()) return false;
    return hay.compare(0, n.size(), n) == 0;
}

}  // namespace

std::optional<std::string> suggest_fallback(const std::string& model,
                                             FailoverReason reason) {
    std::string normalized = to_lower(normalize_model_id(model));

    for (const auto& fb : kFallbacks) {
        if (reason == fb.reason && starts_with(normalized, fb.from)) {
            return std::string(fb.to);
        }
    }
    return std::nullopt;
}

std::optional<std::string> tier_down_for_context(const std::string& model) {
    std::string normalized = to_lower(normalize_model_id(model));

    // Find the current model's context length.
    int64_t current_ctx = -1;
    for (const auto& tier : kContextTiers) {
        if (starts_with(normalized, tier.model)) {
            current_ctx = tier.context_length;
            break;
        }
    }

    if (current_ctx < 0) {
        // Unknown model — try the metadata lookup.
        auto md = fetch_model_metadata(model);
        current_ctx = md.context_length;
    }

    if (current_ctx < 0) return std::nullopt;

    // Find a model with a strictly larger context.
    std::string best;
    int64_t best_ctx = 0;
    for (const auto& tier : kContextTiers) {
        if (tier.context_length > current_ctx) {
            if (best.empty() || tier.context_length < best_ctx) {
                best = tier.model;
                best_ctx = tier.context_length;
            }
        }
    }

    if (best.empty()) return std::nullopt;
    return best;
}

}  // namespace hermes::llm
