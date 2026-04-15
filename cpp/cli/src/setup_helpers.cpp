// C++17 port of the pure-logic primitives behind `hermes setup`.
#include "hermes/cli/setup_helpers.hpp"

#include "hermes/cli/colors.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <iostream>
#include <unistd.h>

namespace hermes::cli::setup_helpers {

namespace {

namespace col = hermes::cli::colors;

std::string lstrip(std::string s) {
    size_t i = 0;
    while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i;
    return s.substr(i);
}

std::string rstrip(std::string s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) {
        s.pop_back();
    }
    return s;
}

std::string strip(std::string s) { return lstrip(rstrip(std::move(s))); }

std::string to_lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return s;
}

bool starts_with(const std::string& s, const std::string& p) {
    return s.size() >= p.size() && s.compare(0, p.size(), p) == 0;
}

bool ends_with(const std::string& s, const std::string& p) {
    return s.size() >= p.size() &&
           s.compare(s.size() - p.size(), p.size(), p) == 0;
}

std::vector<std::string> split(const std::string& s, char sep) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (c == sep) {
            out.push_back(cur);
            cur.clear();
        } else {
            cur.push_back(c);
        }
    }
    out.push_back(cur);
    return out;
}

std::string remove_all(std::string s, char ch) {
    s.erase(std::remove(s.begin(), s.end(), ch), s.end());
    return s;
}

// -------------------------------------------------------------------------
// Default model snapshot — verbatim copy of `_DEFAULT_PROVIDER_MODELS`
// from `hermes_cli/setup.py`.
// -------------------------------------------------------------------------
const std::unordered_map<std::string, std::vector<std::string>>&
default_models_table() {
    static const std::unordered_map<std::string, std::vector<std::string>>
        tbl = {
            {"copilot-acp", {"copilot-acp"}},
            {"copilot",
             {"gpt-5.4", "gpt-5.4-mini", "gpt-5-mini", "gpt-5.3-codex",
              "gpt-5.2-codex", "gpt-4.1", "gpt-4o", "gpt-4o-mini",
              "claude-opus-4.6", "claude-sonnet-4.6", "claude-sonnet-4.5",
              "claude-haiku-4.5", "gemini-2.5-pro", "grok-code-fast-1"}},
            {"gemini",
             {"gemini-3.1-pro-preview", "gemini-3-flash-preview",
              "gemini-3.1-flash-lite-preview", "gemini-2.5-pro",
              "gemini-2.5-flash", "gemini-2.5-flash-lite", "gemma-4-31b-it",
              "gemma-4-26b-it"}},
            {"zai", {"glm-5", "glm-4.7", "glm-4.5", "glm-4.5-flash"}},
            {"kimi-coding",
             {"kimi-k2.5", "kimi-k2-thinking", "kimi-k2-turbo-preview"}},
            {"minimax",
             {"MiniMax-M2.7", "MiniMax-M2.5", "MiniMax-M2.1", "MiniMax-M2"}},
            {"minimax-cn",
             {"MiniMax-M2.7", "MiniMax-M2.5", "MiniMax-M2.1", "MiniMax-M2"}},
            {"ai-gateway",
             {"anthropic/claude-opus-4.6", "anthropic/claude-sonnet-4.6",
              "openai/gpt-5", "google/gemini-3-flash"}},
            {"kilocode",
             {"anthropic/claude-opus-4.6", "anthropic/claude-sonnet-4.6",
              "openai/gpt-5.4", "google/gemini-3-pro-preview",
              "google/gemini-3-flash-preview"}},
            {"opencode-zen",
             {"gpt-5.4", "gpt-5.3-codex", "claude-sonnet-4-6", "gemini-3-flash",
              "glm-5", "kimi-k2.5", "minimax-m2.7"}},
            {"opencode-go",
             {"glm-5", "kimi-k2.5", "mimo-v2-pro", "mimo-v2-omni",
              "minimax-m2.5", "minimax-m2.7"}},
            {"huggingface",
             {"Qwen/Qwen3.5-397B-A17B", "Qwen/Qwen3-235B-A22B-Thinking-2507",
              "Qwen/Qwen3-Coder-480B-A35B-Instruct", "deepseek-ai/DeepSeek-R1-0528",
              "deepseek-ai/DeepSeek-V3.2", "moonshotai/Kimi-K2.5"}},
        };
    return tbl;
}

}  // namespace

// -------------------------------------------------------------------------
// Default models.
// -------------------------------------------------------------------------

const std::vector<std::string>& default_models_for_provider(
    const std::string& provider_id) {
    static const std::vector<std::string> kEmpty;
    auto& tbl = default_models_table();
    auto it = tbl.find(provider_id);
    return it == tbl.end() ? kEmpty : it->second;
}

bool has_default_models(const std::string& provider_id) {
    return default_models_table().count(provider_id) > 0;
}

std::vector<std::string> providers_with_default_models() {
    std::vector<std::string> ids;
    ids.reserve(default_models_table().size());
    for (const auto& kv : default_models_table()) {
        ids.push_back(kv.first);
    }
    std::sort(ids.begin(), ids.end());
    return ids;
}

// -------------------------------------------------------------------------
// Model config dict.
// -------------------------------------------------------------------------

nlohmann::json model_config_dict(const nlohmann::json& config) {
    if (!config.is_object()) return nlohmann::json::object();
    auto it = config.find("model");
    if (it == config.end()) return nlohmann::json::object();
    if (it->is_object()) return *it;
    if (it->is_string()) {
        const std::string s = strip(it->get<std::string>());
        if (s.empty()) return nlohmann::json::object();
        nlohmann::json out = nlohmann::json::object();
        out["default"] = s;
        return out;
    }
    return nlohmann::json::object();
}

void set_default_model(nlohmann::json& config, const std::string& model_name) {
    if (model_name.empty()) return;
    nlohmann::json mcfg = model_config_dict(config);
    mcfg["default"] = model_name;
    config["model"] = mcfg;
}

// -------------------------------------------------------------------------
// Credential pool strategies.
// -------------------------------------------------------------------------

std::unordered_map<std::string, std::string>
get_credential_pool_strategies(const nlohmann::json& config) {
    std::unordered_map<std::string, std::string> out;
    if (!config.is_object()) return out;
    auto it = config.find("credential_pool_strategies");
    if (it == config.end() || !it->is_object()) return out;
    for (auto kv = it->begin(); kv != it->end(); ++kv) {
        if (kv.value().is_string()) {
            out[kv.key()] = kv.value().get<std::string>();
        }
    }
    return out;
}

void set_credential_pool_strategy(nlohmann::json& config,
                                  const std::string& provider,
                                  const std::string& strategy) {
    if (provider.empty()) return;
    auto strategies = get_credential_pool_strategies(config);
    strategies[provider] = strategy;
    nlohmann::json obj = nlohmann::json::object();
    for (const auto& kv : strategies) obj[kv.first] = kv.second;
    config["credential_pool_strategies"] = obj;
}

bool supports_same_provider_pool_setup(const std::string& provider,
                                       const AuthTypeLookup& lookup) {
    if (provider.empty() || provider == "custom") return false;
    if (provider == "openrouter") return true;
    if (!lookup) return false;
    const std::string at = lookup(provider);
    return at == "api_key" || at == "oauth_device_code";
}

// -------------------------------------------------------------------------
// Reasoning effort.
// -------------------------------------------------------------------------

std::string current_reasoning_effort(const nlohmann::json& config) {
    if (!config.is_object()) return "";
    auto it = config.find("agent");
    if (it == config.end() || !it->is_object()) return "";
    auto eit = it->find("reasoning_effort");
    if (eit == it->end() || !eit->is_string()) return "";
    return to_lower(strip(eit->get<std::string>()));
}

void set_reasoning_effort(nlohmann::json& config, const std::string& effort) {
    nlohmann::json agent;
    auto it = config.find("agent");
    if (it != config.end() && it->is_object()) {
        agent = *it;
    } else {
        agent = nlohmann::json::object();
    }
    agent["reasoning_effort"] = effort;
    config["agent"] = agent;
}

ReasoningChoiceMenu build_reasoning_choice_menu(
    const nlohmann::json& config, const std::vector<std::string>& efforts) {
    ReasoningChoiceMenu menu;
    if (efforts.empty()) return menu;

    const std::string current = current_reasoning_effort(config);
    menu.choices = efforts;
    menu.choices.emplace_back("Disable reasoning");
    menu.choices.emplace_back(
        std::string("Keep current (") +
        (current.empty() ? "default" : current) + ")");

    if (current == "none") {
        menu.default_index = static_cast<int>(efforts.size());
    } else {
        auto it = std::find(efforts.begin(), efforts.end(), current);
        if (it != efforts.end()) {
            menu.default_index = static_cast<int>(std::distance(efforts.begin(), it));
        } else {
            auto med = std::find(efforts.begin(), efforts.end(),
                                 std::string("medium"));
            if (med != efforts.end()) {
                menu.default_index =
                    static_cast<int>(std::distance(efforts.begin(), med));
            } else {
                menu.default_index = static_cast<int>(menu.choices.size()) - 1;
            }
        }
    }
    return menu;
}

void apply_reasoning_choice(nlohmann::json& config,
                            const std::vector<std::string>& efforts,
                            int selected_index) {
    if (selected_index < 0) return;
    const int n = static_cast<int>(efforts.size());
    if (selected_index < n) {
        set_reasoning_effort(config, efforts[selected_index]);
    } else if (selected_index == n) {
        set_reasoning_effort(config, "none");
    }
    // selected_index > n  →  "keep current" (no-op)
}

// -------------------------------------------------------------------------
// Section summary.
// -------------------------------------------------------------------------

namespace {

bool truthy(const std::string& s) { return !s.empty(); }

}  // namespace

std::optional<std::string> get_section_config_summary(
    const nlohmann::json& config,
    const std::string& section_key,
    const EnvLookupFn& env_lookup,
    const ActiveProviderLookup& active_provider_lookup) {
    auto get_env = [&](const std::string& name) -> std::string {
        return env_lookup ? env_lookup(name) : std::string{};
    };

    if (section_key == "model") {
        bool has_key =
            truthy(get_env("OPENROUTER_API_KEY")) ||
            truthy(get_env("OPENAI_API_KEY")) ||
            truthy(get_env("ANTHROPIC_API_KEY"));
        if (!has_key && active_provider_lookup) {
            auto active = active_provider_lookup();
            if (active && !active->empty()) has_key = true;
        }
        if (!has_key) return std::nullopt;

        if (config.is_object()) {
            auto it = config.find("model");
            if (it != config.end()) {
                if (it->is_string()) {
                    auto s = strip(it->get<std::string>());
                    if (!s.empty()) return s;
                } else if (it->is_object()) {
                    auto def = it->find("default");
                    if (def != it->end() && def->is_string() &&
                        !def->get<std::string>().empty()) {
                        return def->get<std::string>();
                    }
                    auto mod = it->find("model");
                    if (mod != it->end() && mod->is_string() &&
                        !mod->get<std::string>().empty()) {
                        return mod->get<std::string>();
                    }
                    return std::string("configured");
                }
            }
        }
        return std::string("configured");
    }

    if (section_key == "terminal") {
        std::string backend = "local";
        if (config.is_object()) {
            auto it = config.find("terminal");
            if (it != config.end() && it->is_object()) {
                auto b = it->find("backend");
                if (b != it->end() && b->is_string()) {
                    backend = b->get<std::string>();
                }
            }
        }
        return std::string("backend: ") + backend;
    }

    if (section_key == "agent") {
        long long max_turns = 90;
        if (config.is_object()) {
            auto it = config.find("agent");
            if (it != config.end() && it->is_object()) {
                auto mt = it->find("max_turns");
                if (mt != it->end() && mt->is_number_integer()) {
                    max_turns = mt->get<long long>();
                }
            }
        }
        return std::string("max turns: ") + std::to_string(max_turns);
    }

    if (section_key == "gateway") {
        std::vector<std::string> platforms;
        if (truthy(get_env("TELEGRAM_BOT_TOKEN"))) platforms.emplace_back("Telegram");
        if (truthy(get_env("DISCORD_BOT_TOKEN"))) platforms.emplace_back("Discord");
        if (truthy(get_env("SLACK_BOT_TOKEN"))) platforms.emplace_back("Slack");
        if (truthy(get_env("WHATSAPP_PHONE_NUMBER_ID"))) platforms.emplace_back("WhatsApp");
        if (truthy(get_env("SIGNAL_ACCOUNT"))) platforms.emplace_back("Signal");
        if (truthy(get_env("BLUEBUBBLES_SERVER_URL"))) platforms.emplace_back("BlueBubbles");
        if (platforms.empty()) return std::nullopt;
        std::string out;
        for (size_t i = 0; i < platforms.size(); ++i) {
            if (i) out += ", ";
            out += platforms[i];
        }
        return out;
    }

    if (section_key == "tools") {
        std::vector<std::string> tools;
        if (truthy(get_env("ELEVENLABS_API_KEY"))) tools.emplace_back("TTS/ElevenLabs");
        if (truthy(get_env("BROWSERBASE_API_KEY"))) tools.emplace_back("Browser");
        if (truthy(get_env("FIRECRAWL_API_KEY"))) tools.emplace_back("Firecrawl");
        if (tools.empty()) return std::nullopt;
        std::string out;
        for (size_t i = 0; i < tools.size(); ++i) {
            if (i) out += ", ";
            out += tools[i];
        }
        return out;
    }

    return std::nullopt;
}

// -------------------------------------------------------------------------
// Discord user-id cleanup.
// -------------------------------------------------------------------------

std::vector<std::string> clean_discord_user_ids(const std::string& raw) {
    std::vector<std::string> cleaned;
    std::string deflated = remove_all(raw, ' ');
    for (auto piece : split(deflated, ',')) {
        std::string uid = strip(std::move(piece));
        if (starts_with(uid, "<@") && ends_with(uid, ">")) {
            // Strip leading `<@` and any leading `!` chars; strip trailing `>`.
            size_t i = 0;
            while (i < uid.size() && (uid[i] == '<' || uid[i] == '@' || uid[i] == '!')) ++i;
            size_t j = uid.size();
            while (j > i && uid[j - 1] == '>') --j;
            uid = uid.substr(i, j - i);
        }
        if (uid.size() >= 5 && to_lower(uid.substr(0, 5)) == "user:") {
            uid = uid.substr(5);
        }
        if (!uid.empty()) cleaned.push_back(uid);
    }
    return cleaned;
}

// -------------------------------------------------------------------------
// Pretty-print helpers.
// -------------------------------------------------------------------------

std::string format_header(const std::string& title) {
    return col::cyan(col::bold(std::string("\xE2\x97\x86 ") + title));
}

std::string format_info(const std::string& text) {
    return col::dim(std::string("  ") + text);
}

std::string format_success(const std::string& text) {
    return col::green(std::string("\xE2\x9C\x93 ") + text);
}

std::string format_warning(const std::string& text) {
    return col::yellow(std::string("\xE2\x9A\xA0 ") + text);
}

std::string format_error(const std::string& text) {
    return col::red(std::string("\xE2\x9C\x97 ") + text);
}

void print_header(const std::string& title) {
    std::cout << "\n" << format_header(title) << "\n";
}

void print_info(const std::string& text) {
    std::cout << format_info(text) << "\n";
}

void print_success(const std::string& text) {
    std::cout << format_success(text) << "\n";
}

void print_warning(const std::string& text) {
    std::cout << format_warning(text) << "\n";
}

void print_error(const std::string& text) {
    std::cout << format_error(text) << "\n";
}

bool is_interactive_stdin() {
    return ::isatty(0) != 0;
}

std::string format_noninteractive_setup_guidance(const std::string& reason) {
    std::string out;
    out += "\n";
    out += col::cyan(col::bold(
        "\xE2\x9A\x95 Hermes Setup \xE2\x80\x94 Non-interactive mode"));
    out += "\n\n";
    if (!reason.empty()) {
        out += format_info(reason);
        out += "\n";
    }
    out += format_info("The interactive wizard cannot be used here.");
    out += "\n\n";
    out += format_info("Configure Hermes using environment variables or config commands:");
    out += "\n";
    out += format_info("  hermes config set model.provider custom");
    out += "\n";
    out += format_info("  hermes config set model.base_url http://localhost:8080/v1");
    out += "\n";
    out += format_info("  hermes config set model.default your-model-name");
    out += "\n\n";
    out += format_info("Or set OPENROUTER_API_KEY / OPENAI_API_KEY in your environment.");
    out += "\n";
    out += format_info("Run 'hermes setup' in an interactive terminal to use the full wizard.");
    out += "\n\n";
    return out;
}

void print_noninteractive_setup_guidance(const std::string& reason) {
    std::cout << format_noninteractive_setup_guidance(reason);
}

}  // namespace hermes::cli::setup_helpers
