// Deep model metadata — implementation.
#include "hermes/llm/model_metadata_depth.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <regex>
#include <string>
#include <string_view>
#include <unordered_set>

namespace hermes::llm {

namespace {

std::string to_lower(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        out.push_back(static_cast<char>(
            std::tolower(static_cast<unsigned char>(c))));
    }
    return out;
}

bool starts_with(std::string_view s, std::string_view p) {
    return s.size() >= p.size() && s.compare(0, p.size(), p) == 0;
}

bool contains(std::string_view hay, std::string_view needle) {
    return hay.find(needle) != std::string_view::npos;
}

bool contains_ci(std::string_view hay, std::string_view needle) {
    return contains(to_lower(hay), to_lower(needle));
}

}  // namespace

// ── probe tiers ────────────────────────────────────────────────────────

std::optional<int64_t> get_next_probe_tier(int64_t current) {
    for (int64_t t : kContextProbeTiers) {
        if (t < current) return t;
    }
    return std::nullopt;
}

// ── provider prefixes ──────────────────────────────────────────────────

namespace {
const std::unordered_set<std::string>& known_prefixes() {
    static const std::unordered_set<std::string> kPrefixes = {
        "openrouter", "nous", "openai-codex", "copilot", "copilot-acp",
        "gemini", "zai", "kimi-coding", "minimax", "minimax-cn",
        "anthropic", "deepseek",
        "opencode-zen", "opencode-go", "ai-gateway", "kilocode", "alibaba",
        "qwen-oauth", "custom", "local",
        "google", "google-gemini", "google-ai-studio",
        "glm", "z-ai", "z.ai", "zhipu", "github", "github-copilot",
        "github-models", "kimi", "moonshot", "claude", "deep-seek",
        "opencode", "zen", "go", "vercel", "kilo", "dashscope", "aliyun",
        "qwen", "qwen-portal",
    };
    return kPrefixes;
}

bool looks_like_ollama_tag(std::string_view suffix) {
    // Patterns from _OLLAMA_TAG_PATTERN in model_metadata.py.
    static const std::regex pat(
        R"(^(\d+\.?\d*b|latest|stable|q\d|fp?\d|instruct|chat|coder|vision|text))",
        std::regex::icase);
    std::string s(suffix);
    // trim leading ws
    std::size_t i = 0;
    while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i;
    s = s.substr(i);
    return std::regex_search(s, pat);
}

}  // namespace

bool is_known_provider_prefix(std::string_view p) {
    return known_prefixes().count(to_lower(p)) > 0;
}

std::string strip_model_provider_prefix(std::string_view model) {
    if (model.find(':') == std::string_view::npos) return std::string(model);
    if (starts_with(model, "http")) return std::string(model);
    auto colon = model.find(':');
    std::string_view prefix = model.substr(0, colon);
    std::string_view suffix = model.substr(colon + 1);
    // trim
    while (!prefix.empty() && std::isspace(static_cast<unsigned char>(prefix.front()))) prefix.remove_prefix(1);
    while (!prefix.empty() && std::isspace(static_cast<unsigned char>(prefix.back()))) prefix.remove_suffix(1);
    if (!is_known_provider_prefix(prefix)) return std::string(model);
    if (looks_like_ollama_tag(suffix)) return std::string(model);
    return std::string(suffix);
}

// ── URL→provider inference ────────────────────────────────────────────

namespace {

struct UrlProvider {
    const char* host_substr;
    const char* provider;
};

constexpr std::array<UrlProvider, 20> kUrlProviders = {{
    {"api.openai.com",                "openai"},
    {"chatgpt.com",                   "openai"},
    {"api.anthropic.com",             "anthropic"},
    {"api.z.ai",                      "zai"},
    {"api.moonshot.ai",               "kimi-coding"},
    {"api.kimi.com",                  "kimi-coding"},
    {"api.minimax",                   "minimax"},
    {"dashscope.aliyuncs.com",        "alibaba"},
    {"dashscope-intl.aliyuncs.com",   "alibaba"},
    {"portal.qwen.ai",                "qwen-oauth"},
    {"openrouter.ai",                 "openrouter"},
    {"generativelanguage.googleapis", "gemini"},
    {"inference-api.nousresearch",    "nous"},
    {"api.deepseek.com",              "deepseek"},
    {"api.githubcopilot.com",         "copilot"},
    {"models.github.ai",              "copilot"},
    {"api.fireworks.ai",              "fireworks"},
    {"opencode.ai",                   "opencode-go"},
    {"api.x.ai",                      "xai"},
    {"api.mistral.ai",                "mistral"},
}};

std::string extract_host(std::string_view url) {
    std::string_view s = url;
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.remove_prefix(1);
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.remove_suffix(1);
    auto scheme = s.find("://");
    if (scheme != std::string_view::npos) s.remove_prefix(scheme + 3);
    auto slash = s.find('/');
    if (slash != std::string_view::npos) s = s.substr(0, slash);
    auto at = s.find('@');
    if (at != std::string_view::npos) s = s.substr(at + 1);
    auto colon = s.find(':');
    if (colon != std::string_view::npos) s = s.substr(0, colon);
    std::string out(s);
    for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

}  // namespace

std::string infer_provider_from_base_url(std::string_view base_url) {
    if (base_url.empty()) return {};
    const std::string host = extract_host(base_url);
    for (const auto& up : kUrlProviders) {
        if (host.find(up.host_substr) != std::string::npos) {
            return up.provider;
        }
    }
    return {};
}

bool is_known_provider_base_url(std::string_view base_url) {
    return !infer_provider_from_base_url(base_url).empty();
}

// ── local endpoint detection ──────────────────────────────────────────

bool is_local_endpoint(std::string_view base_url) {
    if (base_url.empty()) return false;
    const std::string host = extract_host(base_url);
    if (host.empty()) return false;
    if (host == "localhost" || host == "::1") return true;
    // IPv4 address parsing.
    int parts[4] = {-1,-1,-1,-1};
    int idx = 0;
    std::string cur;
    for (char c : host) {
        if (c == '.') {
            if (idx >= 4 || cur.empty()) return false;
            try { parts[idx++] = std::stoi(cur); } catch (...) { return false; }
            cur.clear();
        } else if (std::isdigit(static_cast<unsigned char>(c))) {
            cur.push_back(c);
        } else {
            return false;   // not an IPv4
        }
    }
    if (idx == 3 && !cur.empty()) {
        try { parts[idx++] = std::stoi(cur); } catch (...) { return false; }
    }
    if (idx != 4) return false;
    for (int p : parts) {
        if (p < 0 || p > 255) return false;
    }
    if (parts[0] == 127) return true;                          // 127/8
    if (parts[0] == 10)  return true;                          // 10/8
    if (parts[0] == 172 && parts[1] >= 16 && parts[1] <= 31) return true;  // 172.16/12
    if (parts[0] == 192 && parts[1] == 168) return true;       // 192.168/16
    if (parts[0] == 169 && parts[1] == 254) return true;       // link-local
    if (parts[0] == 0 && parts[1] == 0 && parts[2] == 0 && parts[3] == 0) return true;
    return false;
}

std::string_view local_server_type_name(LocalServerType t) {
    switch (t) {
        case LocalServerType::Ollama:   return "ollama";
        case LocalServerType::LmStudio: return "lm-studio";
        case LocalServerType::Vllm:     return "vllm";
        case LocalServerType::LlamaCpp: return "llamacpp";
        case LocalServerType::Unknown:
        default: return "unknown";
    }
}

// ── context-length defaults ───────────────────────────────────────────

const std::vector<ContextEntry>& default_context_table() {
    // Note: order matters — longer / more specific keys should appear
    // before generic catchalls so longest-substring semantics below
    // pick the right one.  In practice we explicitly do longest-prefix
    // matching by length, so ordering is a readability aid only.
    static const std::vector<ContextEntry> kTable = {
        // Claude 4.6 (1M)
        {"claude-opus-4-6",   1000000},
        {"claude-sonnet-4-6", 1000000},
        {"claude-opus-4.6",   1000000},
        {"claude-sonnet-4.6", 1000000},
        // Claude 4.5
        {"claude-opus-4-5",    200000},
        {"claude-sonnet-4-5",  200000},
        {"claude-haiku-4-5",   200000},
        // Claude 4 / 3.x
        {"claude-3-7-sonnet",  200000},
        {"claude-3-5-sonnet",  200000},
        {"claude-3-5-haiku",   200000},
        {"claude-3-opus",      200000},
        {"claude-opus",        200000},
        {"claude-sonnet",      200000},
        {"claude-haiku",       200000},
        {"claude",             200000},
        // OpenAI
        {"gpt-4.1",           1047576},
        {"gpt-5",              400000},
        {"gpt-4o-mini",        128000},
        {"gpt-4o",             128000},
        {"gpt-4",              128000},
        {"o3-mini",            200000},
        {"o3-pro",             200000},
        {"o3",                 200000},
        {"o1-preview",         128000},
        {"o1-mini",            128000},
        {"o1",                 200000},
        // Google
        {"gemini-2.5-pro",    2097152},
        {"gemini-2.5-flash",  1048576},
        {"gemini-3-flash",    1048576},
        {"gemini-2.0-flash",  1000000},
        {"gemini-1.5-pro",    2097152},
        {"gemini-1.5-flash",  1048576},
        {"gemini",            1048576},
        {"gemma-4-31b",        256000},
        {"gemma-4-26b",        256000},
        {"gemma-3",            131072},
        {"gemma",                8192},
        // DeepSeek
        {"deepseek-v3",        128000},
        {"deepseek-r1",        128000},
        {"deepseek-chat",      128000},
        {"deepseek",           128000},
        // Meta
        {"llama-3.3",          131072},
        {"llama-3.2",          131072},
        {"llama-3.1",          131072},
        {"llama",              131072},
        // Mistral
        {"mistral-large",      131072},
        {"mistral-small",       32768},
        {"mistral",             32768},
        // Qwen
        {"qwen3-coder-plus",  1000000},
        {"qwen3-coder",        262144},
        {"qwen3",              131072},
        {"qwen2.5",            131072},
        {"qwen",               131072},
        // MiniMax
        {"minimax-m2",         204800},
        {"minimax",            204800},
        // GLM / Zhipu
        {"glm-4.6",            202752},
        {"glm-5",              202752},
        {"glm-4",              131072},
        {"glm",                202752},
        // xAI Grok
        {"grok-code-fast",     256000},
        {"grok-4-1-fast",     2000000},
        {"grok-2-vision",        8192},
        {"grok-4-fast",       2000000},
        {"grok-4.20",         2000000},
        {"grok-4",             256000},
        {"grok-3",             131072},
        {"grok-2",             131072},
        {"grok",               131072},
        // Kimi / Moonshot
        {"kimi-k2",            262144},
        {"kimi",               262144},
        // Cohere
        {"command-r-plus",     128000},
        {"command-r",          128000},
        // Arcee / Trinity
        {"trinity",            262144},
        // HuggingFace inference
        {"Qwen/Qwen3.5-397B-A17B", 131072},
        {"Qwen/Qwen3.5-35B-A3B",   131072},
        {"deepseek-ai/DeepSeek-V3.2", 65536},
        {"moonshotai/Kimi-K2.5",      262144},
        {"moonshotai/Kimi-K2-Thinking", 262144},
        {"MiniMaxAI/MiniMax-M2.5",  204800},
        {"XiaomiMiMo/MiMo-V2-Flash", 32768},
        {"mimo-v2-pro",       1048576},
        {"mimo-v2-omni",      1048576},
        {"zai-org/GLM-5",      202752},
        // Nous
        {"hermes-4",           128000},
        {"hermes-3",           128000},
        {"hermes",             128000},
        // Codex
        {"gpt-5-codex",        400000},
        {"gpt-5.3-codex",      400000},
        {"codex",              200000},
    };
    return kTable;
}

int64_t lookup_default_context_length(std::string_view model) {
    const std::string m = to_lower(model);
    const auto& table = default_context_table();
    std::size_t best_len = 0;
    int64_t best_val = kDefaultFallbackContext;
    for (const auto& e : table) {
        const std::string k = to_lower(e.key);
        if (m.find(k) != std::string::npos && k.size() > best_len) {
            best_len = k.size();
            best_val = e.context;
        }
    }
    return best_val;
}

// ── family detection ───────────────────────────────────────────────────

std::string_view model_family_name(ModelFamily f) {
    switch (f) {
        case ModelFamily::ClaudeOpus:   return "claude-opus";
        case ModelFamily::ClaudeSonnet: return "claude-sonnet";
        case ModelFamily::ClaudeHaiku:  return "claude-haiku";
        case ModelFamily::Gpt4:         return "gpt-4";
        case ModelFamily::Gpt4o:        return "gpt-4o";
        case ModelFamily::Gpt5:         return "gpt-5";
        case ModelFamily::O1:           return "o1";
        case ModelFamily::O3:           return "o3";
        case ModelFamily::Gemini:       return "gemini";
        case ModelFamily::Gemma:        return "gemma";
        case ModelFamily::DeepSeek:     return "deepseek";
        case ModelFamily::Qwen:         return "qwen";
        case ModelFamily::QwenCoder:    return "qwen-coder";
        case ModelFamily::Llama:        return "llama";
        case ModelFamily::Mistral:      return "mistral";
        case ModelFamily::Minimax:      return "minimax";
        case ModelFamily::Glm:          return "glm";
        case ModelFamily::Kimi:         return "kimi";
        case ModelFamily::Grok:         return "grok";
        case ModelFamily::Hermes:       return "hermes";
        case ModelFamily::Codex:        return "codex";
        case ModelFamily::Unknown:
        default:                        return "unknown";
    }
}

ModelFamily detect_model_family(std::string_view model) {
    const std::string m = to_lower(model);
    if (contains(m, "claude")) {
        if (contains(m, "opus"))   return ModelFamily::ClaudeOpus;
        if (contains(m, "sonnet")) return ModelFamily::ClaudeSonnet;
        if (contains(m, "haiku"))  return ModelFamily::ClaudeHaiku;
        return ModelFamily::ClaudeSonnet;  // default family
    }
    if (contains(m, "codex"))              return ModelFamily::Codex;
    if (contains(m, "gpt-5"))              return ModelFamily::Gpt5;
    if (contains(m, "gpt-4o"))             return ModelFamily::Gpt4o;
    if (contains(m, "gpt-4"))              return ModelFamily::Gpt4;
    if (contains(m, "o3-mini") || contains(m, "o3-pro") || starts_with(m, "o3"))
        return ModelFamily::O3;
    if (starts_with(m, "o1"))              return ModelFamily::O1;
    if (contains(m, "gemma"))              return ModelFamily::Gemma;
    if (contains(m, "gemini"))             return ModelFamily::Gemini;
    if (contains(m, "deepseek"))           return ModelFamily::DeepSeek;
    if (contains(m, "qwen") && contains(m, "coder")) return ModelFamily::QwenCoder;
    if (contains(m, "qwen"))               return ModelFamily::Qwen;
    if (contains(m, "llama"))              return ModelFamily::Llama;
    if (contains(m, "mistral"))            return ModelFamily::Mistral;
    if (contains(m, "minimax"))            return ModelFamily::Minimax;
    if (contains(m, "glm"))                return ModelFamily::Glm;
    if (contains(m, "kimi"))               return ModelFamily::Kimi;
    if (contains(m, "grok"))               return ModelFamily::Grok;
    if (contains(m, "hermes"))             return ModelFamily::Hermes;
    return ModelFamily::Unknown;
}

// ── extended pricing ──────────────────────────────────────────────────

namespace {

struct PricingEntry {
    const char* key;              // longest-prefix substring match
    double prompt_per_mtok;
    double completion_per_mtok;
    double cache_read_per_mtok;
    double cache_write_per_mtok;
    int64_t context_length;
    int64_t max_output;
    ModelFamily family;
    bool supports_vision;
    bool supports_prompt_cache;
    bool supports_1h_cache;
    double cache_1h_write_per_mtok;
};

// Pricing in USD per MILLION tokens.  Cache 5m write is the standard tier;
// cache_1h_write is the extended tier ("1-hour" prompt caching).
const std::vector<PricingEntry>& pricing_entries() {
    static const std::vector<PricingEntry> kP = {
        // ── Anthropic (native) ────────────────────────────────────────
        // Claude 4.6 Opus: input $15, output $75, cache read $1.50,
        // cache write 5m $18.75, cache write 1h $30.  Vision & cache support.
        {"claude-opus-4-6",       15.00, 75.00, 1.50, 18.75, 1000000, 128000,
         ModelFamily::ClaudeOpus,   true, true, true, 30.00},
        {"claude-sonnet-4-6",      3.00, 15.00, 0.30,  3.75, 1000000,  64000,
         ModelFamily::ClaudeSonnet, true, true, true,  6.00},
        {"claude-opus-4-5",       15.00, 75.00, 1.50, 18.75,  200000,  64000,
         ModelFamily::ClaudeOpus,   true, true, true, 30.00},
        {"claude-sonnet-4-5",      3.00, 15.00, 0.30,  3.75,  200000,  64000,
         ModelFamily::ClaudeSonnet, true, true, true,  6.00},
        {"claude-haiku-4-5",       1.00,  5.00, 0.10,  1.25,  200000,  64000,
         ModelFamily::ClaudeHaiku,  true, true, false, 0.00},
        {"claude-3-7-sonnet",      3.00, 15.00, 0.30,  3.75,  200000, 128000,
         ModelFamily::ClaudeSonnet, true, true, false, 0.00},
        {"claude-3-5-sonnet",      3.00, 15.00, 0.30,  3.75,  200000,   8192,
         ModelFamily::ClaudeSonnet, true, true, false, 0.00},
        {"claude-3-5-haiku",       0.80,  4.00, 0.08,  1.00,  200000,   8192,
         ModelFamily::ClaudeHaiku,  false, true, false, 0.00},
        {"claude-3-opus",         15.00, 75.00, 1.50, 18.75,  200000,   4096,
         ModelFamily::ClaudeOpus,   true, true, false, 0.00},
        {"claude-3-sonnet",        3.00, 15.00, 0.30,  3.75,  200000,   4096,
         ModelFamily::ClaudeSonnet, true, false, false, 0.00},
        {"claude-3-haiku",         0.25,  1.25, 0.03,  0.30,  200000,   4096,
         ModelFamily::ClaudeHaiku,  false, true, false, 0.00},

        // ── OpenAI ────────────────────────────────────────────────────
        {"gpt-4.1",                2.00,  8.00, 0.50,  0.00, 1047576,  32768,
         ModelFamily::Gpt4,         true, true, false, 0.00},
        {"gpt-4.1-mini",           0.40,  1.60, 0.10,  0.00, 1047576,  32768,
         ModelFamily::Gpt4,         true, true, false, 0.00},
        {"gpt-4.1-nano",           0.10,  0.40, 0.025, 0.00, 1047576,  32768,
         ModelFamily::Gpt4,         true, true, false, 0.00},
        {"gpt-4o-mini",            0.15,  0.60, 0.075, 0.00,  128000,  16384,
         ModelFamily::Gpt4o,        true, true, false, 0.00},
        {"gpt-4o",                 2.50, 10.00, 1.25,  0.00,  128000,  16384,
         ModelFamily::Gpt4o,        true, true, false, 0.00},
        {"gpt-5",                  1.25, 10.00, 0.125, 0.00,  400000, 128000,
         ModelFamily::Gpt5,         true, true, false, 0.00},
        {"gpt-5-mini",             0.25,  2.00, 0.025, 0.00,  400000, 128000,
         ModelFamily::Gpt5,         true, true, false, 0.00},
        {"gpt-5-nano",             0.05,  0.40, 0.005, 0.00,  400000, 128000,
         ModelFamily::Gpt5,         true, true, false, 0.00},
        {"gpt-5-codex",            1.25, 10.00, 0.125, 0.00,  400000, 128000,
         ModelFamily::Codex,        false, true, false, 0.00},
        {"gpt-5.3-codex",          1.25, 10.00, 0.125, 0.00,  400000, 128000,
         ModelFamily::Codex,        false, true, false, 0.00},
        {"o1-preview",            15.00, 60.00, 7.50,  0.00,  128000,  32768,
         ModelFamily::O1,           false, false, false, 0.00},
        {"o1-mini",                3.00, 12.00, 1.50,  0.00,  128000,  65536,
         ModelFamily::O1,           false, false, false, 0.00},
        {"o1",                    15.00, 60.00, 7.50,  0.00,  200000, 100000,
         ModelFamily::O1,           false, false, false, 0.00},
        {"o3-mini",                1.10,  4.40, 0.55,  0.00,  200000, 100000,
         ModelFamily::O3,           false, false, false, 0.00},
        {"o3-pro",                20.00, 80.00,10.00,  0.00,  200000, 100000,
         ModelFamily::O3,           true, false, false, 0.00},
        {"o3",                     2.00,  8.00, 0.50,  0.00,  200000, 100000,
         ModelFamily::O3,           true, false, false, 0.00},
        {"gpt-4",                 30.00, 60.00, 0.00,  0.00,  128000,   8192,
         ModelFamily::Gpt4,         false, false, false, 0.00},

        // ── Google ────────────────────────────────────────────────────
        {"gemini-2.5-pro",         1.25, 10.00, 0.00,  0.00, 2097152,  65536,
         ModelFamily::Gemini,       true, false, false, 0.00},
        {"gemini-2.5-flash",       0.30,  2.50, 0.00,  0.00, 1048576,  65536,
         ModelFamily::Gemini,       true, false, false, 0.00},
        {"gemini-3-flash",         0.10,  0.40, 0.00,  0.00, 1048576,  32768,
         ModelFamily::Gemini,       true, false, false, 0.00},
        {"gemini-2.0-flash",       0.10,  0.40, 0.00,  0.00, 1000000,   8192,
         ModelFamily::Gemini,       true, false, false, 0.00},
        {"gemini-1.5-pro",         1.25,  5.00, 0.00,  0.00, 2097152,   8192,
         ModelFamily::Gemini,       true, false, false, 0.00},
        {"gemini-1.5-flash",       0.075, 0.30, 0.00,  0.00, 1048576,   8192,
         ModelFamily::Gemini,       true, false, false, 0.00},
        {"gemma-4-31b",            0.00,  0.00, 0.00,  0.00,  256000,   8192,
         ModelFamily::Gemma,        false, false, false, 0.00},
        {"gemma-4-26b",            0.00,  0.00, 0.00,  0.00,  256000,   8192,
         ModelFamily::Gemma,        false, false, false, 0.00},
        {"gemma-3",                0.00,  0.00, 0.00,  0.00,  131072,   8192,
         ModelFamily::Gemma,        false, false, false, 0.00},

        // ── DeepSeek ──────────────────────────────────────────────────
        {"deepseek-v3",            0.27,  1.10, 0.07,  0.00,  128000,   8192,
         ModelFamily::DeepSeek,     false, true, false, 0.00},
        {"deepseek-r1",            0.55,  2.19, 0.14,  0.00,  128000,  32768,
         ModelFamily::DeepSeek,     false, true, false, 0.00},
        {"deepseek-chat",          0.27,  1.10, 0.07,  0.00,  128000,   8192,
         ModelFamily::DeepSeek,     false, true, false, 0.00},
        {"deepseek",               0.27,  1.10, 0.07,  0.00,  128000,   8192,
         ModelFamily::DeepSeek,     false, true, false, 0.00},

        // ── Qwen / Alibaba ────────────────────────────────────────────
        {"qwen3-coder-plus",       2.00, 10.00, 0.00,  0.00, 1000000,  65536,
         ModelFamily::QwenCoder,    false, false, false, 0.00},
        {"qwen3-coder",            2.00, 10.00, 0.00,  0.00,  262144,  65536,
         ModelFamily::QwenCoder,    false, false, false, 0.00},
        {"qwen3",                  0.80,  3.20, 0.00,  0.00,  131072,  32768,
         ModelFamily::Qwen,         false, false, false, 0.00},
        {"qwen2.5",                0.80,  3.20, 0.00,  0.00,  131072,  32768,
         ModelFamily::Qwen,         false, false, false, 0.00},
        {"qwen",                   0.50,  2.00, 0.00,  0.00,  131072,  32768,
         ModelFamily::Qwen,         false, false, false, 0.00},

        // ── Meta / Llama ──────────────────────────────────────────────
        {"llama-3.3",              0.60,  0.60, 0.00,  0.00,  131072,   8192,
         ModelFamily::Llama,        false, false, false, 0.00},
        {"llama-3.1",              0.60,  0.60, 0.00,  0.00,  131072,   8192,
         ModelFamily::Llama,        false, false, false, 0.00},
        {"llama",                  0.60,  0.60, 0.00,  0.00,  131072,   8192,
         ModelFamily::Llama,        false, false, false, 0.00},

        // ── Mistral ───────────────────────────────────────────────────
        {"mistral-large",          2.00,  6.00, 0.00,  0.00,  131072,   8192,
         ModelFamily::Mistral,      false, false, false, 0.00},
        {"mistral-small",          0.20,  0.60, 0.00,  0.00,   32768,   8192,
         ModelFamily::Mistral,      false, false, false, 0.00},
        {"mistral",                0.70,  2.10, 0.00,  0.00,   32768,   8192,
         ModelFamily::Mistral,      false, false, false, 0.00},

        // ── MiniMax ───────────────────────────────────────────────────
        {"minimax-m2",             0.20,  1.20, 0.00,  0.00,  204800, 131072,
         ModelFamily::Minimax,      false, false, false, 0.00},
        {"minimax",                0.20,  1.20, 0.00,  0.00,  204800, 131072,
         ModelFamily::Minimax,      false, false, false, 0.00},

        // ── GLM / Zhipu ───────────────────────────────────────────────
        {"glm-4.6",                0.60,  2.20, 0.00,  0.00,  202752,  65536,
         ModelFamily::Glm,          false, false, false, 0.00},
        {"glm-5",                  0.60,  2.20, 0.00,  0.00,  202752,  65536,
         ModelFamily::Glm,          false, false, false, 0.00},
        {"glm-4.5",                0.60,  2.20, 0.00,  0.00,  131072,  65536,
         ModelFamily::Glm,          false, false, false, 0.00},
        {"glm",                    0.60,  2.20, 0.00,  0.00,  131072,  65536,
         ModelFamily::Glm,          false, false, false, 0.00},

        // ── Kimi / Moonshot ───────────────────────────────────────────
        {"kimi-k2-turbo",          0.30,  1.80, 0.00,  0.00,  262144, 131072,
         ModelFamily::Kimi,         false, false, false, 0.00},
        {"kimi-k2",                0.30,  1.80, 0.00,  0.00,  262144, 131072,
         ModelFamily::Kimi,         false, false, false, 0.00},
        {"kimi",                   0.30,  1.80, 0.00,  0.00,  262144, 131072,
         ModelFamily::Kimi,         false, false, false, 0.00},

        // ── xAI Grok ──────────────────────────────────────────────────
        {"grok-code-fast",         0.20,  1.50, 0.00,  0.00,  256000, 131072,
         ModelFamily::Grok,         false, false, false, 0.00},
        {"grok-4-fast",            0.20,  1.50, 0.00,  0.00, 2000000, 131072,
         ModelFamily::Grok,         false, false, false, 0.00},
        {"grok-4",                 3.00, 15.00, 0.00,  0.00,  256000,  65536,
         ModelFamily::Grok,         true, false, false, 0.00},
        {"grok-3",                 3.00, 15.00, 0.00,  0.00,  131072,  65536,
         ModelFamily::Grok,         false, false, false, 0.00},
        {"grok-2",                 2.00, 10.00, 0.00,  0.00,  131072,  32768,
         ModelFamily::Grok,         true, false, false, 0.00},
        {"grok",                   2.00, 10.00, 0.00,  0.00,  131072,  32768,
         ModelFamily::Grok,         false, false, false, 0.00},

        // ── Nous Hermes / OpenRouter-hosted ───────────────────────────
        {"hermes-4-405b",          0.00,  0.00, 0.00,  0.00,  128000,  32768,
         ModelFamily::Hermes,       false, false, false, 0.00},
        {"hermes-4",               0.00,  0.00, 0.00,  0.00,  128000,  32768,
         ModelFamily::Hermes,       false, false, false, 0.00},
        {"hermes",                 0.00,  0.00, 0.00,  0.00,  128000,  32768,
         ModelFamily::Hermes,       false, false, false, 0.00},

        // ── Cohere ────────────────────────────────────────────────────
        {"command-r-plus",         2.50, 10.00, 0.00,  0.00,  128000,   4096,
         ModelFamily::Unknown,      false, false, false, 0.00},
        {"command-r",              0.15,  0.60, 0.00,  0.00,  128000,   4096,
         ModelFamily::Unknown,      false, false, false, 0.00},

        // ── Codex / OpenAI ChatGPT ────────────────────────────────────
        {"codex",                  1.25, 10.00, 0.125, 0.00,  200000, 128000,
         ModelFamily::Codex,        false, true, false, 0.00},
    };
    return kP;
}

}  // namespace

ExtendedPricing lookup_extended_pricing(std::string_view model) {
    const std::string m = to_lower(model);
    const auto& entries = pricing_entries();
    std::size_t best_len = 0;
    const PricingEntry* best = nullptr;
    for (const auto& e : entries) {
        const std::string k = to_lower(e.key);
        if (m.find(k) != std::string::npos && k.size() > best_len) {
            best_len = k.size();
            best = &e;
        }
    }
    ExtendedPricing out;
    out.family = detect_model_family(model);
    out.context_length = lookup_default_context_length(model);
    if (best) {
        out.prompt_per_mtok = best->prompt_per_mtok;
        out.completion_per_mtok = best->completion_per_mtok;
        out.cache_read_per_mtok = best->cache_read_per_mtok;
        out.cache_write_per_mtok = best->cache_write_per_mtok;
        out.cache_1h_write_per_mtok = best->cache_1h_write_per_mtok;
        out.context_length = best->context_length;
        out.max_output = best->max_output;
        out.family = best->family;
        out.supports_vision = best->supports_vision;
        out.supports_prompt_cache = best->supports_prompt_cache;
        out.supports_extended_cache_1h = best->supports_1h_cache;
        out.has_1h_cache = best->supports_1h_cache &&
                          best->cache_1h_write_per_mtok > 0;
    }
    out.supports_tools = true;
    if (out.max_output == 0) {
        out.max_output = std::min<int64_t>(32768, out.context_length);
    }
    return out;
}

// ── tokenizer mapping ─────────────────────────────────────────────────

std::string_view tokenizer_family_name(TokenizerFamily t) {
    switch (t) {
        case TokenizerFamily::Cl100k:        return "cl100k_base";
        case TokenizerFamily::O200k:         return "o200k_base";
        case TokenizerFamily::Claude:        return "claude";
        case TokenizerFamily::Gemini:        return "gemini";
        case TokenizerFamily::Qwen:          return "qwen";
        case TokenizerFamily::Llama:         return "llama";
        case TokenizerFamily::Mistral:       return "mistral";
        case TokenizerFamily::DeepSeek:      return "deepseek";
        case TokenizerFamily::CharHeuristic: return "char-heuristic";
        case TokenizerFamily::Unknown:
        default:                             return "unknown";
    }
}

TokenizerFamily tokenizer_family_for_model(std::string_view model) {
    const std::string m = to_lower(model);
    if (contains(m, "claude")) return TokenizerFamily::Claude;
    if (contains(m, "gpt-4o") || contains(m, "gpt-5") ||
        starts_with(m, "o1") || starts_with(m, "o3") ||
        contains(m, "codex"))
        return TokenizerFamily::O200k;
    if (contains(m, "gpt-4") || contains(m, "gpt-3"))
        return TokenizerFamily::Cl100k;
    if (contains(m, "gemini") || contains(m, "gemma"))
        return TokenizerFamily::Gemini;
    if (contains(m, "qwen")) return TokenizerFamily::Qwen;
    if (contains(m, "llama")) return TokenizerFamily::Llama;
    if (contains(m, "mistral")) return TokenizerFamily::Mistral;
    if (contains(m, "deepseek")) return TokenizerFamily::DeepSeek;
    return TokenizerFamily::CharHeuristic;
}

// ── budget planner ────────────────────────────────────────────────────

BudgetPlan plan_completion_budget(std::string_view model,
                                  int64_t estimated_prompt_tokens,
                                  int64_t context_length) {
    BudgetPlan plan;
    plan.prompt_tokens = estimated_prompt_tokens;
    auto p = lookup_extended_pricing(model);
    const int64_t ctx = context_length > 0 ? context_length : p.context_length;
    plan.available_context = ctx - estimated_prompt_tokens;
    if (plan.available_context <= 0) {
        plan.overflow = true;
        plan.reserved_output = 0;
        return plan;
    }
    // Slack: reserve at least 256 tokens for formatting overhead.
    constexpr int64_t kSlack = 256;
    const int64_t ceiling = p.max_output > 0 ? p.max_output : 32768;
    int64_t budget = std::min(ceiling, plan.available_context - kSlack);
    if (budget < 512) {
        // Too tight — drop slack to let the caller retry.
        budget = std::max<int64_t>(1, plan.available_context);
    }
    plan.reserved_output = budget;
    return plan;
}

// ── cache key helpers ─────────────────────────────────────────────────

std::string make_context_cache_key(std::string_view model,
                                   std::string_view base_url) {
    std::string out;
    out.reserve(model.size() + base_url.size() + 1);
    out.append(model);
    out.push_back('@');
    out.append(base_url);
    return out;
}

std::optional<ContextCacheKey> parse_context_cache_key(std::string_view key) {
    // Last '@' separates model from URL since URLs never contain @.  Be
    // conservative: use first '@' after any character.
    auto at = key.find('@');
    if (at == std::string_view::npos || at == 0) return std::nullopt;
    ContextCacheKey out;
    out.model = std::string(key.substr(0, at));
    out.base_url = std::string(key.substr(at + 1));
    return out;
}

// ── pricing math ──────────────────────────────────────────────────────

double compute_call_cost_usd(const ExtendedPricing& p,
                             int64_t input_tokens,
                             int64_t output_tokens,
                             int64_t cache_read_tokens,
                             int64_t cache_write_tokens,
                             int64_t cache_1h_write_tokens) {
    auto per_mtok = [](int64_t t, double rate) {
        return (static_cast<double>(t) / 1'000'000.0) * rate;
    };
    double cost = 0;
    cost += per_mtok(input_tokens,         p.prompt_per_mtok);
    cost += per_mtok(output_tokens,        p.completion_per_mtok);
    cost += per_mtok(cache_read_tokens,    p.cache_read_per_mtok);
    cost += per_mtok(cache_write_tokens,   p.cache_write_per_mtok);
    cost += per_mtok(cache_1h_write_tokens, p.cache_1h_write_per_mtok);
    return cost;
}

int64_t estimate_tokens_from_chars(int64_t chars, TokenizerFamily family) {
    if (chars <= 0) return 0;
    // Ratios roughly from OpenAI / Anthropic docs.
    double ratio = 4.0;  // default 4 chars/token
    switch (family) {
        case TokenizerFamily::Cl100k:
        case TokenizerFamily::O200k:
            ratio = 4.0; break;
        case TokenizerFamily::Claude:
            ratio = 3.8; break;
        case TokenizerFamily::Gemini:
            ratio = 4.2; break;
        case TokenizerFamily::Qwen:
            ratio = 3.5; break;   // BPE handles CJK denser
        case TokenizerFamily::Llama:
        case TokenizerFamily::Mistral:
            ratio = 3.8; break;
        case TokenizerFamily::DeepSeek:
            ratio = 3.5; break;
        case TokenizerFamily::CharHeuristic:
        case TokenizerFamily::Unknown:
        default:
            ratio = 4.0; break;
    }
    return static_cast<int64_t>(static_cast<double>(chars) / ratio);
}

}  // namespace hermes::llm
