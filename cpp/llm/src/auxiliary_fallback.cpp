// Auxiliary provider fallback chain — implementation.
#include "hermes/llm/auxiliary_fallback.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <string>
#include <unordered_set>

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

}  // namespace

std::string_view auxiliary_provider_name(AuxiliaryProvider p) {
    switch (p) {
        case AuxiliaryProvider::OpenRouter:  return "openrouter";
        case AuxiliaryProvider::NousPortal:  return "nous-portal";
        case AuxiliaryProvider::Custom:      return "custom";
        case AuxiliaryProvider::CodexOauth:  return "openai-codex";
        case AuxiliaryProvider::Anthropic:   return "anthropic";
        case AuxiliaryProvider::Gemini:      return "gemini";
        case AuxiliaryProvider::Zai:         return "zai";
        case AuxiliaryProvider::KimiCoding:  return "kimi-coding";
        case AuxiliaryProvider::Minimax:     return "minimax";
        case AuxiliaryProvider::MinimaxCn:   return "minimax-cn";
        case AuxiliaryProvider::AiGateway:   return "ai-gateway";
        case AuxiliaryProvider::OpencodeZen: return "opencode-zen";
        case AuxiliaryProvider::OpencodeGo:  return "opencode-go";
        case AuxiliaryProvider::Kilocode:    return "kilocode";
        case AuxiliaryProvider::Unknown:
        default:                             return "unknown";
    }
}

AuxiliaryProvider auxiliary_provider_from_name(std::string_view name) {
    const std::string n = to_lower_copy(name);
    if (n == "openrouter")                       return AuxiliaryProvider::OpenRouter;
    if (n == "nous" || n == "nous-portal")       return AuxiliaryProvider::NousPortal;
    if (n == "custom")                           return AuxiliaryProvider::Custom;
    if (n == "codex" || n == "openai-codex")     return AuxiliaryProvider::CodexOauth;
    if (n == "anthropic" || n == "claude" || n == "claude-code") return AuxiliaryProvider::Anthropic;
    if (n == "gemini" || n == "google" ||
        n == "google-gemini" || n == "google-ai-studio") return AuxiliaryProvider::Gemini;
    if (n == "zai" || n == "z-ai" || n == "z.ai" ||
        n == "glm" || n == "zhipu")              return AuxiliaryProvider::Zai;
    if (n == "kimi" || n == "moonshot" ||
        n == "kimi-coding")                      return AuxiliaryProvider::KimiCoding;
    if (n == "minimax")                          return AuxiliaryProvider::Minimax;
    if (n == "minimax-cn" || n == "minimax_cn" ||
        n == "minimax-china")                    return AuxiliaryProvider::MinimaxCn;
    if (n == "ai-gateway")                       return AuxiliaryProvider::AiGateway;
    if (n == "opencode-zen")                     return AuxiliaryProvider::OpencodeZen;
    if (n == "opencode-go")                      return AuxiliaryProvider::OpencodeGo;
    if (n == "kilocode")                         return AuxiliaryProvider::Kilocode;
    return AuxiliaryProvider::Unknown;
}

// ── error classifiers ───────────────────────────────────────────────────

bool is_credit_exhaustion_error(int http_status, std::string_view body) {
    if (http_status == 402) return true;
    if (http_status != 429 && http_status != 0) {
        // 5xx and other non-402 non-429 → not credit exhaustion.
        if (http_status >= 500 || http_status == 401 || http_status == 403) {
            return false;
        }
    }
    const std::string lower = to_lower_copy(body);
    static constexpr std::array<const char*, 10> kCreditKeywords = {
        "credits", "insufficient funds", "afford", "exceeded your quota",
        "quota exceeded", "billing", "payment required",
        "insufficient balance", "ran out of credits", "payment_required",
    };
    for (const char* kw : kCreditKeywords) {
        if (contains_ci(lower, kw)) return true;
    }
    return false;
}

bool is_connection_error(std::string_view err) {
    const std::string lower = to_lower_copy(err);
    static constexpr std::array<const char*, 10> kKeywords = {
        "connection refused", "connection reset", "connection aborted",
        "timed out", "timeout", "unreachable", "dns", "name resolution",
        "reset by peer", "broken pipe",
    };
    for (const char* kw : kKeywords) {
        if (contains_ci(lower, kw)) return true;
    }
    return false;
}

// ── default models ──────────────────────────────────────────────────────

std::string default_auxiliary_model(AuxiliaryProvider p, bool for_vision) {
    // Mirrors _API_KEY_PROVIDER_AUX_MODELS in auxiliary_client.py.
    switch (p) {
        case AuxiliaryProvider::Gemini:
            return for_vision ? "gemini-3-flash-preview" : "gemini-3-flash-preview";
        case AuxiliaryProvider::Zai:
            return "glm-4.5-flash";
        case AuxiliaryProvider::KimiCoding:
            return "kimi-k2-turbo-preview";
        case AuxiliaryProvider::Minimax:
        case AuxiliaryProvider::MinimaxCn:
            return "MiniMax-M2.7";
        case AuxiliaryProvider::Anthropic:
            return for_vision ? "claude-haiku-4-5-20251001" : "claude-haiku-4-5-20251001";
        case AuxiliaryProvider::AiGateway:
            return "google/gemini-3-flash";
        case AuxiliaryProvider::OpencodeZen:
            return "gemini-3-flash";
        case AuxiliaryProvider::OpencodeGo:
            return "glm-5";
        case AuxiliaryProvider::Kilocode:
            return "google/gemini-3-flash-preview";
        case AuxiliaryProvider::OpenRouter:
            return for_vision ? "google/gemini-3-flash-preview"
                              : "google/gemini-3-flash-preview";
        case AuxiliaryProvider::NousPortal:
            return "hermes-4-405b";
        case AuxiliaryProvider::CodexOauth:
            return "gpt-5.3-codex";
        case AuxiliaryProvider::Custom:
        case AuxiliaryProvider::Unknown:
        default:
            return {};
    }
}

// ── chain building ──────────────────────────────────────────────────────

std::vector<AuxiliaryProvider> build_fallback_chain(
    AuxiliaryProvider main_provider, bool for_vision) {
    // Canonical chain per auxiliary_client.py docstring.
    std::vector<AuxiliaryProvider> text_chain = {
        AuxiliaryProvider::OpenRouter,
        AuxiliaryProvider::NousPortal,
        AuxiliaryProvider::Custom,
        AuxiliaryProvider::CodexOauth,
        AuxiliaryProvider::Anthropic,
        AuxiliaryProvider::Zai,
        AuxiliaryProvider::KimiCoding,
        AuxiliaryProvider::Minimax,
        AuxiliaryProvider::MinimaxCn,
        AuxiliaryProvider::Gemini,
    };
    std::vector<AuxiliaryProvider> vision_chain = {
        AuxiliaryProvider::OpenRouter,
        AuxiliaryProvider::NousPortal,
        AuxiliaryProvider::CodexOauth,
        AuxiliaryProvider::Anthropic,
        AuxiliaryProvider::Custom,
        AuxiliaryProvider::Gemini,
    };

    std::vector<AuxiliaryProvider> result;
    std::unordered_set<int> seen;
    auto push = [&](AuxiliaryProvider p) {
        if (p == AuxiliaryProvider::Unknown) return;
        if (seen.insert(static_cast<int>(p)).second) {
            result.push_back(p);
        }
    };

    if (main_provider != AuxiliaryProvider::Unknown) {
        push(main_provider);
    }
    const auto& chain = for_vision ? vision_chain : text_chain;
    for (auto p : chain) push(p);
    return result;
}

// ── chain execution ─────────────────────────────────────────────────────

FallbackResult run_fallback_chain(
    const std::vector<AuxiliaryProvider>& chain,
    const AttemptDispatcher& dispatch,
    bool for_vision) {
    FallbackResult result;
    if (!dispatch) return result;

    for (auto provider : chain) {
        const std::string model = default_auxiliary_model(provider, for_vision);
        AttemptObservation obs = dispatch(provider, model);
        obs.provider = provider;
        if (obs.model.empty()) obs.model = model;
        result.total_latency += obs.latency;
        result.total_cost_usd += obs.estimated_cost_usd;
        result.history.push_back(obs);
        if (obs.succeeded) {
            result.winning_attempt = obs;
            break;
        }
        // If the error is NOT a credit/connection error, stop advancing —
        // a 400 / 401 won't get better on another provider (except quota).
        const bool advance = (!obs.http_status.has_value() &&
                              is_connection_error(obs.error_message)) ||
            (obs.http_status.has_value() &&
             is_credit_exhaustion_error(*obs.http_status, obs.error_message));
        if (!advance) {
            // Non-transient error — stop.
            break;
        }
    }
    return result;
}

}  // namespace hermes::llm
