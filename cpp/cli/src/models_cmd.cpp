// C++17 port of core helpers of hermes_cli/models.py.
#include "hermes/cli/models_cmd.hpp"

#include "hermes/cli/colors.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <iomanip>
#include <iostream>
#include <map>
#include <sstream>

namespace hermes::cli::models_cmd {

namespace {

namespace col = hermes::cli::colors;

std::string to_lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) {
                       return static_cast<char>(std::tolower(c));
                   });
    return s;
}

// -----------------------------------------------------------------
// Provider alias table.  Matches `_PROVIDER_ALIASES` in Python.
// -----------------------------------------------------------------
const std::unordered_map<std::string, std::string>& alias_table() {
    static const std::unordered_map<std::string, std::string> tbl = {
        {"glm", "zai"},
        {"z-ai", "zai"},
        {"z.ai", "zai"},
        {"zhipu", "zai"},
        {"github", "copilot"},
        {"github-copilot", "copilot"},
        {"github-models", "copilot"},
        {"github-model", "copilot"},
        {"github-copilot-acp", "copilot-acp"},
        {"copilot-acp-agent", "copilot-acp"},
        {"google", "gemini"},
        {"google-gemini", "gemini"},
        {"google-ai-studio", "gemini"},
        {"kimi", "kimi-coding"},
        {"moonshot", "kimi-coding"},
        {"minimax-china", "minimax-cn"},
        {"minimax_cn", "minimax-cn"},
        {"claude", "anthropic"},
        {"claude-code", "anthropic"},
        {"deep-seek", "deepseek"},
        {"opencode", "opencode-zen"},
        {"zen", "opencode-zen"},
        {"go", "opencode-go"},
        {"aigateway", "ai-gateway"},
        {"vercel", "ai-gateway"},
        {"vercel-ai-gateway", "ai-gateway"},
        {"kilo", "kilocode"},
        {"kilo-code", "kilocode"},
        {"kilo-gateway", "kilocode"},
        {"dashscope", "alibaba"},
        {"aliyun", "alibaba"},
        {"qwen", "alibaba"},
        {"alibaba-cloud", "alibaba"},
        {"qwen-portal", "qwen-oauth"},
        {"hf", "huggingface"},
        {"hugging-face", "huggingface"},
        {"huggingface-hub", "huggingface"},
    };
    return tbl;
}

const std::unordered_map<std::string, std::string>& label_table() {
    static const std::unordered_map<std::string, std::string> tbl = {
        {"openrouter", "OpenRouter"},
        {"openai-codex", "OpenAI Codex"},
        {"copilot-acp", "GitHub Copilot ACP"},
        {"nous", "Nous Portal"},
        {"copilot", "GitHub Copilot"},
        {"gemini", "Google AI Studio"},
        {"zai", "Z.AI / GLM"},
        {"kimi-coding", "Kimi / Moonshot"},
        {"minimax", "MiniMax"},
        {"minimax-cn", "MiniMax (China)"},
        {"anthropic", "Anthropic"},
        {"deepseek", "DeepSeek"},
        {"opencode-zen", "OpenCode Zen"},
        {"opencode-go", "OpenCode Go"},
        {"ai-gateway", "AI Gateway"},
        {"kilocode", "Kilo Code"},
        {"alibaba", "Alibaba Cloud (DashScope)"},
        {"qwen-oauth", "Qwen OAuth (Portal)"},
        {"huggingface", "Hugging Face"},
        {"custom", "Custom endpoint"},
        {"openai", "OpenAI"},
        {"xai", "xAI Grok"},
        {"moonshot", "Moonshot"},
    };
    return tbl;
}

// -----------------------------------------------------------------
// Provider -> static model catalog.  Mirrors `_PROVIDER_MODELS`.
// -----------------------------------------------------------------
const std::map<std::string, std::vector<std::string>>& provider_models() {
    static const std::map<std::string, std::vector<std::string>> tbl = {
        {"nous", {
            "anthropic/claude-opus-4.6",
            "anthropic/claude-sonnet-4.6",
            "anthropic/claude-sonnet-4.5",
            "anthropic/claude-haiku-4.5",
            "openai/gpt-5.4",
            "openai/gpt-5.4-mini",
            "xiaomi/mimo-v2-pro",
            "openai/gpt-5.3-codex",
            "google/gemini-3-pro-preview",
            "google/gemini-3-flash-preview",
            "google/gemini-3.1-pro-preview",
            "google/gemini-3.1-flash-lite-preview",
            "qwen/qwen3.5-plus-02-15",
            "qwen/qwen3.5-35b-a3b",
            "stepfun/step-3.5-flash",
            "minimax/minimax-m2.7",
            "minimax/minimax-m2.5",
            "z-ai/glm-5.1",
            "z-ai/glm-5-turbo",
            "moonshotai/kimi-k2.5",
            "x-ai/grok-4.20-beta",
            "nvidia/nemotron-3-super-120b-a12b",
            "nvidia/nemotron-3-super-120b-a12b:free",
            "arcee-ai/trinity-large-preview:free",
            "arcee-ai/trinity-large-thinking",
            "openai/gpt-5.4-pro",
            "openai/gpt-5.4-nano",
        }},
        {"openai-codex", {
            "gpt-5.4", "gpt-5.4-mini", "gpt-5.3-codex", "gpt-5.2-codex",
            "gpt-5.1-codex-mini", "gpt-5.1-codex-max",
        }},
        {"copilot-acp", {"copilot-acp"}},
        {"copilot", {
            "gpt-5.4", "gpt-5.4-mini", "gpt-5-mini", "gpt-5.3-codex",
            "gpt-5.2-codex", "gpt-4.1", "gpt-4o", "gpt-4o-mini",
            "claude-opus-4.6", "claude-sonnet-4.6", "claude-sonnet-4.5",
            "claude-haiku-4.5", "gemini-2.5-pro", "grok-code-fast-1",
        }},
        {"gemini", {
            "gemini-3.1-pro-preview", "gemini-3-flash-preview",
            "gemini-3.1-flash-lite-preview", "gemini-2.5-pro",
            "gemini-2.5-flash", "gemini-2.5-flash-lite",
            "gemma-4-31b-it", "gemma-4-26b-it",
        }},
        {"zai", {
            "glm-5", "glm-5-turbo", "glm-4.7", "glm-4.5", "glm-4.5-flash",
        }},
        {"xai", {
            "grok-4.20-0309-reasoning", "grok-4.20-0309-non-reasoning",
            "grok-4.20-multi-agent-0309", "grok-4-1-fast-reasoning",
            "grok-4-1-fast-non-reasoning", "grok-4-fast-reasoning",
            "grok-4-fast-non-reasoning", "grok-4-0709",
            "grok-code-fast-1", "grok-3", "grok-3-mini",
        }},
        {"kimi-coding", {
            "kimi-for-coding", "kimi-k2.5", "kimi-k2-thinking",
            "kimi-k2-thinking-turbo", "kimi-k2-turbo-preview",
            "kimi-k2-0905-preview",
        }},
        {"moonshot", {
            "kimi-k2.5", "kimi-k2-thinking", "kimi-k2-turbo-preview",
            "kimi-k2-0905-preview",
        }},
        {"minimax", {
            "MiniMax-M2.7", "MiniMax-M2.5", "MiniMax-M2.1", "MiniMax-M2",
        }},
        {"minimax-cn", {
            "MiniMax-M2.7", "MiniMax-M2.5", "MiniMax-M2.1", "MiniMax-M2",
        }},
        {"anthropic", {
            "claude-opus-4-6", "claude-sonnet-4-6",
            "claude-opus-4-5-20251101", "claude-sonnet-4-5-20250929",
            "claude-opus-4-20250514", "claude-sonnet-4-20250514",
            "claude-haiku-4-5-20251001",
        }},
        {"deepseek", {"deepseek-chat", "deepseek-reasoner"}},
        {"opencode-zen", {
            "gpt-5.4-pro", "gpt-5.4", "gpt-5.3-codex",
            "gpt-5.3-codex-spark", "gpt-5.2", "gpt-5.2-codex", "gpt-5.1",
            "gpt-5.1-codex", "gpt-5.1-codex-max", "gpt-5.1-codex-mini",
            "gpt-5", "gpt-5-codex", "gpt-5-nano",
            "claude-opus-4-6", "claude-sonnet-4-6",
        }},
        {"openrouter", {
            "anthropic/claude-opus-4.6", "anthropic/claude-sonnet-4.6",
            "qwen/qwen3.6-plus", "anthropic/claude-sonnet-4.5",
            "anthropic/claude-haiku-4.5",
            "openai/gpt-5.4", "openai/gpt-5.4-mini",
            "xiaomi/mimo-v2-pro", "openai/gpt-5.3-codex",
            "google/gemini-3-pro-image-preview",
            "google/gemini-3-flash-preview",
            "google/gemini-3.1-pro-preview",
            "google/gemini-3.1-flash-lite-preview",
            "qwen/qwen3.5-plus-02-15", "qwen/qwen3.5-35b-a3b",
            "stepfun/step-3.5-flash", "minimax/minimax-m2.7",
            "minimax/minimax-m2.5", "z-ai/glm-5.1", "z-ai/glm-5-turbo",
            "moonshotai/kimi-k2.5", "x-ai/grok-4.20",
            "nvidia/nemotron-3-super-120b-a12b",
            "nvidia/nemotron-3-super-120b-a12b:free",
            "arcee-ai/trinity-large-preview:free",
            "arcee-ai/trinity-large-thinking", "openai/gpt-5.4-pro",
            "openai/gpt-5.4-nano",
        }},
        {"openai", {
            "gpt-5.4", "gpt-5.4-mini", "gpt-5.4-pro",
            "gpt-4.1", "gpt-4o", "gpt-4o-mini",
            "o3", "o3-mini", "o4-mini",
        }},
    };
    return tbl;
}

// Static pricing + context hints — per-million-token USD prices,
// approximate; used for CLI display only.
struct ModelHints {
    int ctx = 0;
    double prompt_usd = 0.0;
    double completion_usd = 0.0;
};
const std::unordered_map<std::string, ModelHints>& hint_table() {
    static const std::unordered_map<std::string, ModelHints> tbl = {
        {"anthropic/claude-opus-4.6",       {200000, 15.0, 75.0}},
        {"anthropic/claude-sonnet-4.6",     {200000,  3.0, 15.0}},
        {"anthropic/claude-haiku-4.5",      {200000,  1.0,  5.0}},
        {"claude-opus-4-6",                 {200000, 15.0, 75.0}},
        {"claude-sonnet-4-6",               {200000,  3.0, 15.0}},
        {"openai/gpt-5.4",                  {128000,  2.5, 10.0}},
        {"openai/gpt-5.4-mini",             {128000,  0.3,  1.2}},
        {"gpt-5.4",                         {128000,  2.5, 10.0}},
        {"gpt-5.4-mini",                    {128000,  0.3,  1.2}},
        {"google/gemini-3-pro-preview",     {1000000, 1.25, 5.0}},
        {"gemini-2.5-pro",                  {1000000, 1.25, 5.0}},
        {"gemini-2.5-flash",                {1000000, 0.075, 0.30}},
        {"moonshotai/kimi-k2.5",            {200000,  0.15,  0.60}},
        {"x-ai/grok-4.20",                  {131072,  2.0,   8.0}},
        {"deepseek-chat",                   {64000,   0.14,  0.28}},
        {"deepseek-reasoner",               {64000,   0.55,  2.19}},
    };
    return tbl;
}

bool contains_id(const std::vector<std::string>& cat, const std::string& id) {
    return std::find(cat.begin(), cat.end(), id) != cat.end();
}

std::string fmt_price(double usd) {
    if (usd <= 0.0) return "-";
    std::ostringstream os;
    os << "$" << std::fixed << std::setprecision(usd < 1.0 ? 3 : 2) << usd;
    return os.str();
}

std::string fmt_ctx(int ctx) {
    if (ctx <= 0) return "-";
    if (ctx >= 1000000) {
        std::ostringstream os;
        os << (ctx / 1000000) << "M";
        return os.str();
    }
    if (ctx >= 1000) {
        std::ostringstream os;
        os << (ctx / 1000) << "K";
        return os.str();
    }
    return std::to_string(ctx);
}

}  // namespace

std::vector<CuratedModel> curated_models_for_provider(
    const std::string& provider) {
    const auto& tbl = provider_models();
    auto it = tbl.find(normalize_provider(provider));
    if (it == tbl.end()) return {};
    std::vector<CuratedModel> out;
    for (std::size_t i = 0; i < it->second.size(); ++i) {
        CuratedModel c;
        c.id = it->second[i];
        if (i == 0) c.description = "recommended";
        if (c.id.find(":free") != std::string::npos) c.description = "free";
        out.push_back(std::move(c));
    }
    return out;
}

std::vector<std::string> known_providers() {
    std::vector<std::string> out;
    for (const auto& [k, _] : provider_models()) out.push_back(k);
    std::sort(out.begin(), out.end());
    return out;
}

bool is_known_provider(const std::string& provider) {
    if (provider.empty()) return false;
    std::string canon = normalize_provider(provider);
    return provider_models().count(canon) > 0 ||
           label_table().count(canon) > 0;
}

std::string normalize_provider(const std::string& provider) {
    std::string lower = to_lower(provider);
    auto it = alias_table().find(lower);
    if (it != alias_table().end()) return it->second;
    return lower;
}

std::string provider_label(const std::string& provider) {
    std::string canon = normalize_provider(provider);
    auto it = label_table().find(canon);
    if (it != label_table().end()) return it->second;
    return canon.empty() ? "(none)" : canon;
}

std::string strip_vendor_prefix(const std::string& model_id) {
    auto slash = model_id.find('/');
    if (slash == std::string::npos) return model_id;
    return model_id.substr(slash + 1);
}

ParsedModel parse_model_input(const std::string& raw,
                              const std::string& current_provider) {
    ParsedModel r;
    if (raw.empty()) {
        r.provider = current_provider;
        return r;
    }
    auto colon = raw.find(':');
    // "provider:model" — but don't split on "foo:free" style IDs.
    if (colon != std::string::npos && colon > 0 &&
        is_known_provider(raw.substr(0, colon))) {
        r.provider = normalize_provider(raw.substr(0, colon));
        r.model = raw.substr(colon + 1);
        return r;
    }
    // "provider/model"
    auto slash = raw.find('/');
    if (slash != std::string::npos && slash > 0 &&
        is_known_provider(raw.substr(0, slash))) {
        r.provider = normalize_provider(raw.substr(0, slash));
        r.model = raw.substr(slash + 1);
        return r;
    }
    r.provider = current_provider;
    r.model = raw;
    return r;
}

std::string detect_provider_for_model(const std::string& model_id) {
    for (const auto& [provider, ids] : provider_models()) {
        if (contains_id(ids, model_id)) return provider;
    }
    std::string stripped = strip_vendor_prefix(model_id);
    for (const auto& [provider, ids] : provider_models()) {
        for (const auto& id : ids) {
            if (strip_vendor_prefix(id) == stripped) return provider;
        }
    }
    return "";
}

int context_window_for_model(const std::string& model_id) {
    const auto& t = hint_table();
    auto it = t.find(model_id);
    if (it != t.end()) return it->second.ctx;
    auto it2 = t.find(strip_vendor_prefix(model_id));
    if (it2 != t.end()) return it2->second.ctx;
    return 0;
}

Pricing pricing_for_model(const std::string& model_id) {
    Pricing out;
    const auto& t = hint_table();
    auto it = t.find(model_id);
    if (it == t.end()) {
        it = t.find(strip_vendor_prefix(model_id));
    }
    if (it != t.end()) {
        out.prompt_per_mtok = it->second.prompt_usd;
        out.completion_per_mtok = it->second.completion_usd;
    }
    return out;
}

bool model_supports_fast_mode(const std::string& model_id) {
    // `claude-*-haiku-*` & `*-mini`, `*-flash-lite` style models are
    // cheap enough to qualify.  Rough heuristic mirror of the Python.
    std::string lower = to_lower(model_id);
    if (lower.find("haiku") != std::string::npos) return true;
    if (lower.find("-mini") != std::string::npos) return true;
    if (lower.find("flash-lite") != std::string::npos) return true;
    if (lower.find("nano") != std::string::npos) return true;
    return false;
}

// -----------------------------------------------------------------
// `hermes models` entry
// -----------------------------------------------------------------
namespace {

void print_models_for_provider(const std::string& provider) {
    const auto models = curated_models_for_provider(provider);
    if (models.empty()) return;
    std::cout << "\n" << col::bold(col::cyan(provider_label(provider) +
                                             "  (" + provider + ")")) << "\n";
    std::cout << "  " << std::left << std::setw(44) << "Model"
              << std::setw(8) << "Ctx"
              << std::setw(10) << "In"
              << std::setw(10) << "Out"
              << "Note\n";
    std::cout << "  " << std::string(70, '-') << "\n";
    for (const auto& m : models) {
        auto p = pricing_for_model(m.id);
        int ctx = context_window_for_model(m.id);
        std::cout << "  " << std::left << std::setw(44) << m.id.substr(0, 43)
                  << std::setw(8) << fmt_ctx(ctx)
                  << std::setw(10) << fmt_price(p.prompt_per_mtok)
                  << std::setw(10) << fmt_price(p.completion_per_mtok)
                  << m.description << "\n";
    }
}

}  // namespace

int run(int argc, char* argv[]) {
    std::string provider_filter;
    bool show_all = false;
    for (int i = 2; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--provider" && i + 1 < argc) {
            provider_filter = argv[++i];
        } else if (a == "--all" || a == "-a") {
            show_all = true;
        } else if (a == "--help" || a == "-h") {
            std::cout << "Usage: hermes models [--provider NAME] [--all]\n\n"
                      << "Lists curated model catalog entries with static\n"
                      << "context window + pricing hints.  Providers:\n";
            for (const auto& p : known_providers()) {
                std::cout << "  " << p << "\n";
            }
            return 0;
        }
    }
    (void)show_all;
    if (!provider_filter.empty()) {
        std::string canon = normalize_provider(provider_filter);
        if (provider_models().count(canon) == 0) {
            std::cerr << "Unknown provider: " << provider_filter << "\n";
            return 1;
        }
        print_models_for_provider(canon);
        std::cout << "\n";
        return 0;
    }
    for (const auto& provider : known_providers()) {
        print_models_for_provider(provider);
    }
    std::cout << "\n";
    return 0;
}

}  // namespace hermes::cli::models_cmd
