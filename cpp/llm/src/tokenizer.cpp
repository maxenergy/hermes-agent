// Family-aware tokenizer and expanded model registry.
//
// Port of agent/model_metadata.py's tokenizer + pricing + context-window
// tables.  All data is hardcoded — Phase 4 overlays live models.dev data.
#include "hermes/llm/tokenizer.hpp"

#include "hermes/llm/llm_client.hpp"
#include "hermes/llm/model_metadata.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <string>
#include <string_view>

namespace hermes::llm {

namespace {

std::string to_lower_copy(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        out.push_back(static_cast<char>(
            std::tolower(static_cast<unsigned char>(c))));
    }
    return out;
}

bool contains_ci(std::string_view hay, std::string_view needle) {
    if (needle.empty()) return true;
    if (hay.size() < needle.size()) return false;
    for (std::size_t i = 0; i + needle.size() <= hay.size(); ++i) {
        bool ok = true;
        for (std::size_t j = 0; j < needle.size(); ++j) {
            const char a = static_cast<char>(
                std::tolower(static_cast<unsigned char>(hay[i + j])));
            const char b = static_cast<char>(
                std::tolower(static_cast<unsigned char>(needle[j])));
            if (a != b) { ok = false; break; }
        }
        if (ok) return true;
    }
    return false;
}

bool starts_with_ci(std::string_view hay, std::string_view needle) {
    if (hay.size() < needle.size()) return false;
    for (std::size_t i = 0; i < needle.size(); ++i) {
        const char a = static_cast<char>(
            std::tolower(static_cast<unsigned char>(hay[i])));
        const char b = static_cast<char>(
            std::tolower(static_cast<unsigned char>(needle[i])));
        if (a != b) return false;
    }
    return true;
}

}  // namespace

std::string_view family_name(ModelFamily f) {
    switch (f) {
        case ModelFamily::Claude:   return "claude";
        case ModelFamily::Gpt:      return "gpt";
        case ModelFamily::Gemini:   return "gemini";
        case ModelFamily::Llama:    return "llama";
        case ModelFamily::Mistral:  return "mistral";
        case ModelFamily::Qwen:     return "qwen";
        case ModelFamily::Deepseek: return "deepseek";
        case ModelFamily::Glm:      return "glm";
        case ModelFamily::Kimi:     return "kimi";
        case ModelFamily::Minimax:  return "minimax";
        case ModelFamily::Grok:     return "grok";
        case ModelFamily::Phi:      return "phi";
        case ModelFamily::Command:  return "command";
        case ModelFamily::Yi:       return "yi";
        case ModelFamily::Unknown:
        default:                    return "unknown";
    }
}

ModelFamily family_of(std::string_view model) {
    const std::string stripped = strip_provider_prefix(model);
    const std::string m = to_lower_copy(stripped);
    if (contains_ci(m, "claude"))                    return ModelFamily::Claude;
    if (contains_ci(m, "gpt") || starts_with_ci(m, "o1") ||
        starts_with_ci(m, "o3") || starts_with_ci(m, "o4") ||
        contains_ci(m, "codex"))                     return ModelFamily::Gpt;
    if (contains_ci(m, "gemini") || contains_ci(m, "palm")) return ModelFamily::Gemini;
    if (contains_ci(m, "llama"))                     return ModelFamily::Llama;
    if (contains_ci(m, "mistral") || contains_ci(m, "mixtral") ||
        contains_ci(m, "codestral"))                 return ModelFamily::Mistral;
    if (contains_ci(m, "qwen"))                      return ModelFamily::Qwen;
    if (contains_ci(m, "deepseek"))                  return ModelFamily::Deepseek;
    if (contains_ci(m, "glm"))                       return ModelFamily::Glm;
    if (contains_ci(m, "kimi") || contains_ci(m, "moonshot")) return ModelFamily::Kimi;
    if (contains_ci(m, "minimax"))                   return ModelFamily::Minimax;
    if (contains_ci(m, "grok"))                      return ModelFamily::Grok;
    if (contains_ci(m, "phi-"))                      return ModelFamily::Phi;
    if (contains_ci(m, "command"))                   return ModelFamily::Command;
    if (starts_with_ci(m, "yi-") || starts_with_ci(m, "yi:")) return ModelFamily::Yi;
    return ModelFamily::Unknown;
}

double chars_per_token(ModelFamily f) {
    switch (f) {
        case ModelFamily::Claude:   return 3.5;
        case ModelFamily::Gpt:      return 4.0;
        case ModelFamily::Gemini:   return 4.2;
        case ModelFamily::Llama:    return 3.8;
        case ModelFamily::Mistral:  return 3.8;
        case ModelFamily::Qwen:     return 2.8;
        case ModelFamily::Deepseek: return 3.5;
        case ModelFamily::Glm:      return 3.2;
        case ModelFamily::Kimi:     return 3.0;
        case ModelFamily::Minimax:  return 3.0;
        case ModelFamily::Grok:     return 4.0;
        case ModelFamily::Phi:      return 4.0;
        case ModelFamily::Command:  return 4.0;
        case ModelFamily::Yi:       return 3.2;
        case ModelFamily::Unknown:
        default:                    return 4.0;
    }
}

int64_t estimate_tokens(std::string_view text, ModelFamily family) {
    if (text.empty()) return 0;
    const double ratio = chars_per_token(family);
    if (ratio <= 0.0) return static_cast<int64_t>(text.size()) / 4;
    const double approx = static_cast<double>(text.size()) / ratio;
    return static_cast<int64_t>(std::ceil(approx));
}

int64_t count_tokens_messages(const std::vector<Message>& messages,
                              ModelFamily family) {
    // Mirrors OpenAI cookbook heuristic: 4 tokens framing per message,
    // plus 2 tokens priming the assistant reply.
    constexpr int64_t per_message_overhead = 4;
    constexpr int64_t reply_priming = 2;

    int64_t total = 0;
    for (const auto& m : messages) {
        total += per_message_overhead;
        total += estimate_tokens(m.content_text, family);
        for (const auto& b : m.content_blocks) {
            total += estimate_tokens(b.text, family);
            if (!b.extra.is_null()) {
                total += estimate_tokens(b.extra.dump(), family);
            }
        }
        for (const auto& tc : m.tool_calls) {
            total += estimate_tokens(tc.name, family);
            if (!tc.arguments.is_null()) {
                total += estimate_tokens(tc.arguments.dump(), family);
            }
        }
        if (m.reasoning) {
            total += estimate_tokens(*m.reasoning, family);
        }
    }
    total += reply_priming;
    return total;
}

// ── richer model registry ───────────────────────────────────────────────

namespace {

struct Entry {
    const char* key;
    ModelFamily family;
    int64_t context;
    int64_t max_output;
    bool reasoning;
    bool vision;
    bool prompt_cache;
    bool json_mode;
    PricingTier pricing;
};

constexpr std::array<Entry, 32> kRegistry = {{
    // ── Anthropic Claude ────────────────────────────────────────────────
    {"claude-opus-4-6",   ModelFamily::Claude, 1'000'000, 128'000, true,  true,  true, true,
     {15.00, 75.00, 1.50, 18.75}},
    {"claude-sonnet-4-6", ModelFamily::Claude, 1'000'000,  64'000, true,  true,  true, true,
     {3.00, 15.00, 0.30, 3.75}},
    {"claude-haiku-4-5",  ModelFamily::Claude,   200'000,   8'192, false, true,  true, true,
     {0.80, 4.00, 0.08, 1.00}},
    {"claude-opus-4",     ModelFamily::Claude,   200'000,  32'000, true,  true,  true, true,
     {15.00, 75.00, 1.50, 18.75}},
    {"claude-sonnet-4",   ModelFamily::Claude,   200'000,  16'000, true,  true,  true, true,
     {3.00, 15.00, 0.30, 3.75}},
    {"claude-3-5-sonnet", ModelFamily::Claude,   200'000,   8'192, false, true,  true, true,
     {3.00, 15.00, 0.30, 3.75}},
    {"claude-3-5-haiku",  ModelFamily::Claude,   200'000,   8'192, false, true,  true, true,
     {0.80, 4.00, 0.08, 1.00}},
    {"claude-3-opus",     ModelFamily::Claude,   200'000,   4'096, false, true,  true, true,
     {15.00, 75.00, 1.50, 18.75}},

    // ── OpenAI ──────────────────────────────────────────────────────────
    {"gpt-5",             ModelFamily::Gpt,      400'000, 128'000, true,  true,  false, true,
     {10.00, 40.00, 0.00, 0.00}},
    {"gpt-5-mini",        ModelFamily::Gpt,      400'000, 128'000, true,  true,  false, true,
     {1.25, 5.00, 0.00, 0.00}},
    {"gpt-4.1",           ModelFamily::Gpt,    1'047'576,  32'768, false, true,  false, true,
     {2.00, 8.00, 0.50, 0.00}},
    {"gpt-4.1-mini",      ModelFamily::Gpt,    1'047'576,  32'768, false, true,  false, true,
     {0.40, 1.60, 0.10, 0.00}},
    {"gpt-4o",            ModelFamily::Gpt,      128'000,  16'384, false, true,  false, true,
     {2.50, 10.00, 1.25, 0.00}},
    {"gpt-4o-mini",       ModelFamily::Gpt,      128'000,  16'384, false, true,  false, true,
     {0.15, 0.60, 0.075, 0.00}},
    {"o1",                ModelFamily::Gpt,      200'000, 100'000, true,  true,  false, false,
     {15.00, 60.00, 7.50, 0.00}},
    {"o3",                ModelFamily::Gpt,      200'000, 100'000, true,  true,  false, false,
     {2.00, 8.00, 0.50, 0.00}},
    {"o3-mini",           ModelFamily::Gpt,      200'000, 100'000, true,  false, false, false,
     {1.10, 4.40, 0.55, 0.00}},
    {"o4-mini",           ModelFamily::Gpt,      200'000, 100'000, true,  true,  false, false,
     {1.10, 4.40, 0.275, 0.00}},

    // ── Google Gemini ───────────────────────────────────────────────────
    {"gemini-2.5-pro",    ModelFamily::Gemini, 2'000'000,  64'000, true,  true,  false, true,
     {1.25, 10.00, 0.3125, 0.00}},
    {"gemini-2.0-flash",  ModelFamily::Gemini, 1'000'000,   8'192, false, true,  false, true,
     {0.10, 0.40, 0.025, 0.00}},
    {"gemini-3-flash",    ModelFamily::Gemini, 1'000'000,   8'192, false, true,  false, true,
     {0.15, 0.60, 0.0375, 0.00}},

    // ── DeepSeek ────────────────────────────────────────────────────────
    {"deepseek-chat",     ModelFamily::Deepseek,  64'000,   8'000, false, false, false, true,
     {0.27, 1.10, 0.07, 0.00}},
    {"deepseek-reasoner", ModelFamily::Deepseek,  64'000,  16'000, true,  false, false, true,
     {0.55, 2.19, 0.14, 0.00}},

    // ── Alibaba Qwen ────────────────────────────────────────────────────
    {"qwen3-max",         ModelFamily::Qwen,     131'072,   8'192, false, true,  false, true,
     {1.20, 4.80, 0.00, 0.00}},
    {"qwen-plus",         ModelFamily::Qwen,     131'072,   8'192, false, true,  false, true,
     {0.40, 1.20, 0.00, 0.00}},
    {"qwen-turbo",        ModelFamily::Qwen,     131'072,   8'192, false, false, false, true,
     {0.05, 0.20, 0.00, 0.00}},

    // ── z.ai / GLM ──────────────────────────────────────────────────────
    {"glm-4.5",           ModelFamily::Glm,      131'072,   8'192, false, false, false, true,
     {0.60, 2.20, 0.00, 0.00}},
    {"glm-4.5-flash",     ModelFamily::Glm,      131'072,   8'192, false, false, false, true,
     {0.00, 0.00, 0.00, 0.00}},

    // ── Moonshot Kimi ───────────────────────────────────────────────────
    {"kimi-k2",           ModelFamily::Kimi,     128'000,   8'192, false, false, false, true,
     {0.60, 2.50, 0.00, 0.00}},
    {"kimi-k2-turbo-preview", ModelFamily::Kimi, 128'000,   8'192, false, false, false, true,
     {1.00, 5.00, 0.00, 0.00}},

    // ── MiniMax ─────────────────────────────────────────────────────────
    {"minimax-m2",        ModelFamily::Minimax,  245'760,   8'192, false, false, false, true,
     {0.30, 1.20, 0.00, 0.00}},

    // ── xAI Grok ────────────────────────────────────────────────────────
    {"grok-4",            ModelFamily::Grok,     256'000,   8'192, true,  true,  false, true,
     {3.00, 15.00, 0.00, 0.00}},
}};

}  // namespace

ModelInfo lookup_model_info(std::string_view model) {
    const std::string stripped = strip_provider_prefix(model);
    ModelInfo out;
    out.model_id = stripped;
    out.family = family_of(stripped);

    const Entry* best = nullptr;
    std::size_t best_len = 0;
    for (const auto& e : kRegistry) {
        const std::string_view key(e.key);
        if (contains_ci(stripped, key) && key.size() > best_len) {
            best = &e;
            best_len = key.size();
        }
    }
    if (best) {
        out.family = best->family;
        out.context_length = best->context;
        out.max_output_tokens = best->max_output;
        out.supports_reasoning = best->reasoning;
        out.supports_vision = best->vision;
        out.supports_prompt_cache = best->prompt_cache;
        out.supports_json_mode = best->json_mode;
        out.pricing = best->pricing;
    } else {
        // Family-level defaults.
        switch (out.family) {
            case ModelFamily::Claude:
                out.context_length = 200'000; out.max_output_tokens = 8192;
                out.supports_prompt_cache = true; out.supports_vision = true;
                break;
            case ModelFamily::Gpt:
                out.context_length = 128'000; out.max_output_tokens = 16384;
                out.supports_vision = true;
                break;
            case ModelFamily::Gemini:
                out.context_length = 1'000'000; out.max_output_tokens = 8192;
                out.supports_vision = true;
                break;
            case ModelFamily::Llama:
                out.context_length = 128'000; out.max_output_tokens = 8192;
                break;
            case ModelFamily::Qwen:
                out.context_length = 131'072; out.max_output_tokens = 8192;
                break;
            case ModelFamily::Deepseek:
                out.context_length = 64'000; out.max_output_tokens = 8000;
                break;
            case ModelFamily::Mistral:
                out.context_length = 128'000; out.max_output_tokens = 8192;
                break;
            case ModelFamily::Glm:
                out.context_length = 131'072; out.max_output_tokens = 8192;
                break;
            case ModelFamily::Kimi:
                out.context_length = 128'000; out.max_output_tokens = 8192;
                break;
            case ModelFamily::Minimax:
                out.context_length = 245'760; out.max_output_tokens = 8192;
                break;
            case ModelFamily::Grok:
                out.context_length = 256'000; out.max_output_tokens = 8192;
                break;
            case ModelFamily::Phi:
                out.context_length = 128'000; out.max_output_tokens = 4096;
                break;
            case ModelFamily::Command:
                out.context_length = 128'000; out.max_output_tokens = 4096;
                break;
            case ModelFamily::Yi:
                out.context_length = 200'000; out.max_output_tokens = 4096;
                break;
            case ModelFamily::Unknown:
            default:
                out.context_length = 8'192; out.max_output_tokens = 2048;
                break;
        }
    }
    return out;
}

std::vector<std::string> list_known_models() {
    std::vector<std::string> out;
    out.reserve(kRegistry.size());
    for (const auto& e : kRegistry) out.emplace_back(e.key);
    return out;
}

// ── budget planner ──────────────────────────────────────────────────────

BudgetPlan plan_budget(std::string_view model,
                       int64_t desired_output_tokens,
                       int64_t safety_margin) {
    BudgetPlan plan;
    plan.safety_margin = std::max<int64_t>(0, safety_margin);

    ModelInfo info = lookup_model_info(model);
    int64_t context = info.context_length;
    if (context <= 0) context = 128'000;
    int64_t native_out = info.max_output_tokens;
    if (native_out <= 0) native_out = 16'384;

    int64_t output = desired_output_tokens;
    if (output <= 0) output = native_out;
    if (output > native_out) {
        output = native_out;
        plan.clamped = true;
    }
    if (output > context - plan.safety_margin) {
        output = std::max<int64_t>(1, context - plan.safety_margin - 1);
        plan.clamped = true;
    }

    plan.output_budget = output;
    plan.input_budget = std::max<int64_t>(0, context - output - plan.safety_margin);
    return plan;
}

}  // namespace hermes::llm
