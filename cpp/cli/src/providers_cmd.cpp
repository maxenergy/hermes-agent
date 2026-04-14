// C++17 port of hermes_cli/providers.py — provider overlays, aliases,
// and the --provider flag resolution chain.
#include "hermes/cli/providers_cmd.hpp"

#include <algorithm>
#include <cctype>
#include <sstream>

namespace hermes::cli::providers_cmd {

namespace {

std::string to_lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

std::string trim(const std::string& s) {
    auto b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return "";
    auto e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

// Labels overriding whatever models.dev would hand us.
const std::unordered_map<std::string, std::string>& label_overrides() {
    static const std::unordered_map<std::string, std::string> tbl = {
        {"nous",         "Nous Portal"},
        {"openai-codex", "OpenAI Codex"},
        {"copilot-acp",  "GitHub Copilot ACP"},
        {"local",        "Local endpoint"},
    };
    return tbl;
}

}  // namespace

const std::unordered_map<std::string, HermesOverlay>& hermes_overlays() {
    static const std::unordered_map<std::string, HermesOverlay> tbl = [] {
        std::unordered_map<std::string, HermesOverlay> m;
        auto add = [&](const std::string& key, HermesOverlay o) { m.emplace(key, std::move(o)); };

        HermesOverlay o_openrouter;
        o_openrouter.transport = "openai_chat";
        o_openrouter.is_aggregator = true;
        o_openrouter.extra_env_vars = {"OPENAI_API_KEY"};
        o_openrouter.base_url_env_var = "OPENROUTER_BASE_URL";
        add("openrouter", o_openrouter);

        HermesOverlay o_nous;
        o_nous.transport = "openai_chat";
        o_nous.auth_type = "oauth_device_code";
        o_nous.base_url_override = "https://inference-api.nousresearch.com/v1";
        add("nous", o_nous);

        HermesOverlay o_codex;
        o_codex.transport = "codex_responses";
        o_codex.auth_type = "oauth_external";
        o_codex.base_url_override = "https://chatgpt.com/backend-api/codex";
        add("openai-codex", o_codex);

        HermesOverlay o_qwen;
        o_qwen.transport = "openai_chat";
        o_qwen.auth_type = "oauth_external";
        o_qwen.base_url_override = "https://portal.qwen.ai/v1";
        o_qwen.base_url_env_var = "HERMES_QWEN_BASE_URL";
        add("qwen-oauth", o_qwen);

        HermesOverlay o_copilot_acp;
        o_copilot_acp.transport = "codex_responses";
        o_copilot_acp.auth_type = "external_process";
        o_copilot_acp.base_url_override = "acp://copilot";
        o_copilot_acp.base_url_env_var = "COPILOT_ACP_BASE_URL";
        add("copilot-acp", o_copilot_acp);

        HermesOverlay o_copilot;
        o_copilot.transport = "openai_chat";
        o_copilot.extra_env_vars = {"COPILOT_GITHUB_TOKEN", "GH_TOKEN"};
        add("github-copilot", o_copilot);

        HermesOverlay o_anthropic;
        o_anthropic.transport = "anthropic_messages";
        o_anthropic.extra_env_vars = {"ANTHROPIC_TOKEN", "CLAUDE_CODE_OAUTH_TOKEN"};
        add("anthropic", o_anthropic);

        HermesOverlay o_zai;
        o_zai.transport = "openai_chat";
        o_zai.extra_env_vars = {"GLM_API_KEY", "ZAI_API_KEY", "Z_AI_API_KEY"};
        o_zai.base_url_env_var = "GLM_BASE_URL";
        add("zai", o_zai);

        HermesOverlay o_kimi;
        o_kimi.transport = "openai_chat";
        o_kimi.base_url_env_var = "KIMI_BASE_URL";
        add("kimi-for-coding", o_kimi);

        HermesOverlay o_minimax;
        o_minimax.transport = "anthropic_messages";
        o_minimax.base_url_env_var = "MINIMAX_BASE_URL";
        add("minimax", o_minimax);

        HermesOverlay o_minimax_cn;
        o_minimax_cn.transport = "anthropic_messages";
        o_minimax_cn.base_url_env_var = "MINIMAX_CN_BASE_URL";
        add("minimax-cn", o_minimax_cn);

        HermesOverlay o_deepseek;
        o_deepseek.transport = "openai_chat";
        o_deepseek.base_url_env_var = "DEEPSEEK_BASE_URL";
        add("deepseek", o_deepseek);

        HermesOverlay o_alibaba;
        o_alibaba.transport = "openai_chat";
        o_alibaba.base_url_env_var = "DASHSCOPE_BASE_URL";
        add("alibaba", o_alibaba);

        HermesOverlay o_vercel;
        o_vercel.transport = "openai_chat";
        o_vercel.is_aggregator = true;
        add("vercel", o_vercel);

        HermesOverlay o_opencode;
        o_opencode.transport = "openai_chat";
        o_opencode.is_aggregator = true;
        o_opencode.base_url_env_var = "OPENCODE_ZEN_BASE_URL";
        add("opencode", o_opencode);

        HermesOverlay o_opencode_go;
        o_opencode_go.transport = "openai_chat";
        o_opencode_go.is_aggregator = true;
        o_opencode_go.base_url_env_var = "OPENCODE_GO_BASE_URL";
        add("opencode-go", o_opencode_go);

        HermesOverlay o_kilo;
        o_kilo.transport = "openai_chat";
        o_kilo.is_aggregator = true;
        o_kilo.base_url_env_var = "KILOCODE_BASE_URL";
        add("kilo", o_kilo);

        HermesOverlay o_hf;
        o_hf.transport = "openai_chat";
        o_hf.is_aggregator = true;
        o_hf.base_url_env_var = "HF_BASE_URL";
        add("huggingface", o_hf);

        HermesOverlay o_xai;
        o_xai.transport = "openai_chat";
        o_xai.base_url_override = "https://api.x.ai/v1";
        o_xai.base_url_env_var = "XAI_BASE_URL";
        add("xai", o_xai);

        return m;
    }();
    return tbl;
}

const std::unordered_map<std::string, std::string>& aliases() {
    static const std::unordered_map<std::string, std::string> tbl = {
        // openrouter
        {"openai", "openrouter"},
        // zai
        {"glm", "zai"}, {"z-ai", "zai"}, {"z.ai", "zai"}, {"zhipu", "zai"},
        // xai
        {"x-ai", "xai"}, {"x.ai", "xai"},
        // kimi-for-coding
        {"kimi", "kimi-for-coding"}, {"kimi-coding", "kimi-for-coding"},
        {"moonshot", "kimi-for-coding"},
        // minimax-cn
        {"minimax-china", "minimax-cn"}, {"minimax_cn", "minimax-cn"},
        // anthropic
        {"claude", "anthropic"}, {"claude-code", "anthropic"},
        // github-copilot
        {"copilot", "github-copilot"}, {"github", "github-copilot"},
        {"github-copilot-acp", "copilot-acp"},
        // vercel
        {"ai-gateway", "vercel"}, {"aigateway", "vercel"},
        {"vercel-ai-gateway", "vercel"},
        // opencode
        {"opencode-zen", "opencode"}, {"zen", "opencode"},
        // opencode-go
        {"go", "opencode-go"}, {"opencode-go-sub", "opencode-go"},
        // kilo
        {"kilocode", "kilo"}, {"kilo-code", "kilo"}, {"kilo-gateway", "kilo"},
        // deepseek
        {"deep-seek", "deepseek"},
        // alibaba
        {"dashscope", "alibaba"}, {"aliyun", "alibaba"},
        {"qwen", "alibaba"}, {"alibaba-cloud", "alibaba"},
        // huggingface
        {"hf", "huggingface"}, {"hugging-face", "huggingface"},
        {"huggingface-hub", "huggingface"},
        // local endpoint aliases
        {"lmstudio", "lmstudio"}, {"lm-studio", "lmstudio"}, {"lm_studio", "lmstudio"},
        {"ollama", "ollama-cloud"},
        {"vllm", "local"}, {"llamacpp", "local"},
        {"llama.cpp", "local"}, {"llama-cpp", "local"},
    };
    return tbl;
}

const std::unordered_map<std::string, std::string>& transport_to_api_mode() {
    static const std::unordered_map<std::string, std::string> tbl = {
        {"openai_chat",        "chat_completions"},
        {"anthropic_messages", "anthropic_messages"},
        {"codex_responses",    "codex_responses"},
    };
    return tbl;
}

std::string normalize_provider(const std::string& name) {
    std::string key = to_lower(trim(name));
    auto it = aliases().find(key);
    if (it != aliases().end()) return it->second;
    return key;
}

std::optional<ProviderDef> get_provider(const std::string& name) {
    std::string canonical = normalize_provider(name);
    auto it = hermes_overlays().find(canonical);
    if (it == hermes_overlays().end()) return std::nullopt;
    const auto& overlay = it->second;

    ProviderDef p;
    p.id = canonical;
    auto lbl_it = label_overrides().find(canonical);
    p.name = (lbl_it != label_overrides().end()) ? lbl_it->second : canonical;
    p.transport = overlay.transport;
    p.api_key_env_vars = overlay.extra_env_vars;
    p.base_url = overlay.base_url_override;
    p.base_url_env_var = overlay.base_url_env_var;
    p.is_aggregator = overlay.is_aggregator;
    p.auth_type = overlay.auth_type;
    p.source = "hermes";
    return p;
}

std::string get_label(const std::string& provider_id) {
    std::string canonical = normalize_provider(provider_id);
    auto lo = label_overrides().find(canonical);
    if (lo != label_overrides().end()) return lo->second;
    auto pdef = get_provider(canonical);
    if (pdef) return pdef->name;
    return canonical;
}

bool is_aggregator(const std::string& provider) {
    auto pdef = get_provider(provider);
    return pdef ? pdef->is_aggregator : false;
}

std::string determine_api_mode(const std::string& provider,
                               const std::string& base_url) {
    auto pdef = get_provider(provider);
    if (pdef) {
        auto it = transport_to_api_mode().find(pdef->transport);
        if (it != transport_to_api_mode().end()) return it->second;
    }
    if (!base_url.empty()) {
        std::string url = base_url;
        while (!url.empty() && url.back() == '/') url.pop_back();
        url = to_lower(url);
        // "/anthropic" suffix → Anthropic Messages.
        const std::string suffix = "/anthropic";
        bool ends_with_anthropic =
            url.size() >= suffix.size() &&
            url.compare(url.size() - suffix.size(), suffix.size(), suffix) == 0;
        if (ends_with_anthropic || url.find("api.anthropic.com") != std::string::npos) {
            return "anthropic_messages";
        }
        if (url.find("api.openai.com") != std::string::npos) {
            return "codex_responses";
        }
    }
    return "chat_completions";
}

std::optional<ProviderDef> resolve_user_provider(const std::string& name,
                                                 const nlohmann::json& user_config) {
    if (!user_config.is_object()) return std::nullopt;
    auto it = user_config.find(name);
    if (it == user_config.end() || !it->is_object()) return std::nullopt;

    auto get_str = [&](const char* k) -> std::string {
        auto v = it->find(k);
        if (v != it->end() && v->is_string()) return v->get<std::string>();
        return "";
    };

    std::string display_name = get_str("name");
    if (display_name.empty()) display_name = name;
    std::string api = get_str("api");
    if (api.empty()) api = get_str("url");
    if (api.empty()) api = get_str("base_url");
    std::string key_env = get_str("key_env");
    std::string transport = get_str("transport");
    if (transport.empty()) transport = "openai_chat";

    ProviderDef p;
    p.id = name;
    p.name = display_name;
    p.transport = transport;
    if (!key_env.empty()) p.api_key_env_vars.push_back(key_env);
    p.base_url = api;
    p.is_aggregator = false;
    p.auth_type = "api_key";
    p.source = "user-config";
    return p;
}

std::string custom_provider_slug(const std::string& display_name) {
    std::string s = trim(display_name);
    s = to_lower(s);
    std::string out;
    out.reserve(s.size() + 8);
    out.append("custom:");
    for (char c : s) {
        out.push_back(c == ' ' ? '-' : c);
    }
    return out;
}

std::optional<ProviderDef> resolve_custom_provider(const std::string& name,
                                                   const nlohmann::json& custom_providers) {
    if (!custom_providers.is_array()) return std::nullopt;
    std::string requested = to_lower(trim(name));
    if (requested.empty()) return std::nullopt;

    for (const auto& entry : custom_providers) {
        if (!entry.is_object()) continue;
        auto get_str = [&](const char* k) -> std::string {
            auto v = entry.find(k);
            if (v != entry.end() && v->is_string()) return v->get<std::string>();
            return "";
        };
        std::string display_name = trim(get_str("name"));
        std::string api = trim(get_str("base_url"));
        if (api.empty()) api = trim(get_str("url"));
        if (api.empty()) api = trim(get_str("api"));
        if (display_name.empty() || api.empty()) continue;

        std::string slug = custom_provider_slug(display_name);
        std::string dn_lower = to_lower(display_name);
        if (requested != dn_lower && requested != slug) continue;

        ProviderDef p;
        p.id = slug;
        p.name = display_name;
        p.transport = "openai_chat";
        p.base_url = api;
        p.is_aggregator = false;
        p.auth_type = "api_key";
        p.source = "user-config";
        return p;
    }
    return std::nullopt;
}

std::optional<ProviderDef> resolve_provider_full(const std::string& name,
                                                 const nlohmann::json& user_providers,
                                                 const nlohmann::json& custom_providers) {
    std::string canonical = normalize_provider(name);

    // 1. Built-in overlay.
    if (auto p = get_provider(canonical)) return p;

    // 2. User-defined providers from config.
    if (user_providers.is_object()) {
        if (auto p = resolve_user_provider(canonical, user_providers)) return p;
        std::string lower_orig = to_lower(trim(name));
        if (lower_orig != canonical) {
            if (auto p = resolve_user_provider(lower_orig, user_providers)) return p;
        }
    }

    // 3. Custom providers from config (flat array).
    if (auto p = resolve_custom_provider(name, custom_providers)) return p;

    return std::nullopt;
}

std::vector<std::string> list_overlay_providers() {
    std::vector<std::string> out;
    out.reserve(hermes_overlays().size());
    for (const auto& kv : hermes_overlays()) out.push_back(kv.first);
    std::sort(out.begin(), out.end());
    return out;
}

std::vector<std::string> api_key_env_vars_for(const std::string& provider) {
    auto pdef = get_provider(provider);
    if (!pdef) return {};
    return pdef->api_key_env_vars;
}

}  // namespace hermes::cli::providers_cmd
