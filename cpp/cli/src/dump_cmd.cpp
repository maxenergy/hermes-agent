// C++17 port of the pure-logic helpers from `hermes_cli/dump.py`.

#include "hermes/cli/dump_cmd.hpp"

#include <algorithm>
#include <sstream>
#include <stdexcept>

namespace hermes::cli::dump_cmd {
namespace {

// Render a JSON scalar / container the way Python's `str()` does for
// the simple cases used by `_config_overrides` and the dump output.
// Specifically:
//   * strings  -> raw text, no quotes
//   * bools    -> "True" / "False"
//   * ints     -> decimal
//   * floats   -> default JSON dump
//   * arrays   -> Python-style `['a', 'b']`
//   * objects  -> Python-style `{'key': 'value'}`
//   * null     -> "None"
std::string python_repr(const nlohmann::json& value);

std::string python_list_repr(const nlohmann::json& array) {
    std::ostringstream oss{};
    oss << "[";
    bool first{true};
    for (const auto& item : array) {
        if (!first) {
            oss << ", ";
        }
        first = false;
        if (item.is_string()) {
            oss << "'" << item.get<std::string>() << "'";
        } else {
            oss << python_repr(item);
        }
    }
    oss << "]";
    return oss.str();
}

std::string python_object_repr(const nlohmann::json& obj) {
    std::ostringstream oss{};
    oss << "{";
    bool first{true};
    for (auto it = obj.begin(); it != obj.end(); ++it) {
        if (!first) {
            oss << ", ";
        }
        first = false;
        oss << "'" << it.key() << "': ";
        if (it->is_string()) {
            oss << "'" << it->get<std::string>() << "'";
        } else {
            oss << python_repr(*it);
        }
    }
    oss << "}";
    return oss.str();
}

std::string python_repr(const nlohmann::json& value) {
    if (value.is_null()) {
        return std::string{"None"};
    }
    if (value.is_boolean()) {
        return value.get<bool>() ? std::string{"True"} : std::string{"False"};
    }
    if (value.is_string()) {
        return value.get<std::string>();
    }
    if (value.is_number_integer()) {
        return std::to_string(value.get<long long>());
    }
    if (value.is_number_float()) {
        std::ostringstream oss{};
        oss << value.get<double>();
        return oss.str();
    }
    if (value.is_array()) {
        return python_list_repr(value);
    }
    if (value.is_object()) {
        return python_object_repr(value);
    }
    return value.dump();
}

std::string pad_label(const std::string& label, std::size_t width) {
    if (label.size() >= width) {
        return label;
    }
    return label + std::string(width - label.size(), ' ');
}

}  // namespace

std::string redact_secret(const std::string& value) {
    if (value.empty()) {
        return std::string{};
    }
    if (value.size() < 12) {
        return std::string{"***"};
    }
    return value.substr(0, 4) + std::string{"..."} +
           value.substr(value.size() - 4);
}

std::size_t count_mcp_servers(const nlohmann::json& config) {
    if (!config.is_object()) {
        return 0;
    }
    auto mcp_it = config.find("mcp");
    if (mcp_it == config.end() || !mcp_it->is_object()) {
        return 0;
    }
    auto servers_it = mcp_it->find("servers");
    if (servers_it == mcp_it->end() || !servers_it->is_object()) {
        return 0;
    }
    return servers_it->size();
}

std::string cron_summary(bool file_exists, const std::string& json_body) {
    if (!file_exists) {
        return std::string{"0"};
    }
    nlohmann::json doc{};
    try {
        doc = nlohmann::json::parse(json_body);
    } catch (const std::exception&) {
        return std::string{"(error reading)"};
    }

    if (!doc.is_object()) {
        return std::string{"(error reading)"};
    }
    auto jobs_it = doc.find("jobs");
    if (jobs_it == doc.end()) {
        return std::string{"0 active / 0 total"};
    }
    if (!jobs_it->is_array()) {
        return std::string{"(error reading)"};
    }

    std::size_t total{jobs_it->size()};
    std::size_t active{0};
    for (const auto& job : *jobs_it) {
        bool enabled{true};
        if (job.is_object()) {
            auto e_it = job.find("enabled");
            if (e_it != job.end() && e_it->is_boolean()) {
                enabled = e_it->get<bool>();
            }
        }
        if (enabled) {
            ++active;
        }
    }

    std::ostringstream oss{};
    oss << active << " active / " << total << " total";
    return oss.str();
}

model_and_provider extract_model_and_provider(const nlohmann::json& config) {
    model_and_provider out{std::string{"(not set)"}, std::string{"(auto)"}};

    if (!config.is_object()) {
        return out;
    }
    auto m_it = config.find("model");
    if (m_it == config.end()) {
        return out;
    }
    if (m_it->is_object()) {
        auto get_first_nonempty =
            [&](std::initializer_list<const char*> keys) -> std::string {
            for (const char* key : keys) {
                auto it = m_it->find(key);
                if (it != m_it->end() && it->is_string()) {
                    std::string value{it->get<std::string>()};
                    if (!value.empty()) {
                        return value;
                    }
                }
            }
            return std::string{};
        };

        std::string model{get_first_nonempty({"default", "model", "name"})};
        if (!model.empty()) {
            out.model = model;
        }
        auto prov_it = m_it->find("provider");
        if (prov_it != m_it->end() && prov_it->is_string()) {
            std::string prov{prov_it->get<std::string>()};
            if (!prov.empty()) {
                out.provider = prov;
            }
        }
        return out;
    }
    if (m_it->is_string()) {
        std::string value{m_it->get<std::string>()};
        if (!value.empty()) {
            out.model = value;
        }
        return out;
    }
    return out;
}

std::string detect_memory_provider(const nlohmann::json& config) {
    if (!config.is_object()) {
        return std::string{"built-in"};
    }
    auto mem_it = config.find("memory");
    if (mem_it == config.end() || !mem_it->is_object()) {
        return std::string{"built-in"};
    }
    auto prov_it = mem_it->find("provider");
    if (prov_it == mem_it->end() || !prov_it->is_string()) {
        return std::string{"built-in"};
    }
    std::string value{prov_it->get<std::string>()};
    if (value.empty()) {
        return std::string{"built-in"};
    }
    return value;
}

const std::vector<std::pair<std::string, std::string>>& platform_env_table() {
    static const std::vector<std::pair<std::string, std::string>> table{
        {"telegram", "TELEGRAM_BOT_TOKEN"},
        {"discord", "DISCORD_BOT_TOKEN"},
        {"slack", "SLACK_BOT_TOKEN"},
        {"whatsapp", "WHATSAPP_ENABLED"},
        {"signal", "SIGNAL_HTTP_URL"},
        {"email", "EMAIL_ADDRESS"},
        {"sms", "TWILIO_ACCOUNT_SID"},
        {"matrix", "MATRIX_HOMESERVER_URL"},
        {"mattermost", "MATTERMOST_URL"},
        {"homeassistant", "HASS_TOKEN"},
        {"dingtalk", "DINGTALK_CLIENT_ID"},
        {"feishu", "FEISHU_APP_ID"},
        {"wecom", "WECOM_BOT_ID"},
        {"weixin", "WEIXIN_ACCOUNT_ID"},
    };
    return table;
}

std::vector<std::string> detect_configured_platforms(
    const env_lookup_fn& env_lookup) {
    std::vector<std::string> out{};
    if (!env_lookup) {
        return out;
    }
    for (const auto& [name, env_var] : platform_env_table()) {
        std::string value{env_lookup(env_var)};
        if (!value.empty()) {
            out.push_back(name);
        }
    }
    return out;
}

const std::vector<std::pair<std::string, std::string>>& api_keys_table() {
    static const std::vector<std::pair<std::string, std::string>> table{
        {"OPENROUTER_API_KEY", "openrouter"},
        {"OPENAI_API_KEY", "openai"},
        {"ANTHROPIC_API_KEY", "anthropic"},
        {"ANTHROPIC_TOKEN", "anthropic_token"},
        {"NOUS_API_KEY", "nous"},
        {"GLM_API_KEY", "glm/zai"},
        {"ZAI_API_KEY", "zai"},
        {"KIMI_API_KEY", "kimi"},
        {"MINIMAX_API_KEY", "minimax"},
        {"DEEPSEEK_API_KEY", "deepseek"},
        {"DASHSCOPE_API_KEY", "dashscope"},
        {"HF_TOKEN", "huggingface"},
        {"AI_GATEWAY_API_KEY", "ai_gateway"},
        {"OPENCODE_ZEN_API_KEY", "opencode_zen"},
        {"OPENCODE_GO_API_KEY", "opencode_go"},
        {"KILOCODE_API_KEY", "kilocode"},
        {"FIRECRAWL_API_KEY", "firecrawl"},
        {"TAVILY_API_KEY", "tavily"},
        {"BROWSERBASE_API_KEY", "browserbase"},
        {"FAL_KEY", "fal"},
        {"ELEVENLABS_API_KEY", "elevenlabs"},
        {"GITHUB_TOKEN", "github"},
    };
    return table;
}

std::string render_dump_api_key_line(const std::string& label,
                                     const std::string& value,
                                     bool show_keys) {
    std::string display{};
    if (show_keys && !value.empty()) {
        display = redact_secret(value);
    } else {
        display = value.empty() ? std::string{"not set"} : std::string{"set"};
    }
    std::ostringstream oss{};
    oss << "  " << pad_label(label, 20) << " " << display;
    return oss.str();
}

std::string format_version_line(const std::string& version,
                                const std::string& release_date,
                                const std::string& commit) {
    std::ostringstream oss{};
    oss << version;
    if (!release_date.empty()) {
        oss << " (" << release_date << ")";
    }
    oss << " [" << commit << "]";
    return oss.str();
}

const std::vector<std::pair<std::string, std::string>>&
interesting_override_paths() {
    static const std::vector<std::pair<std::string, std::string>> paths{
        {"agent", "max_turns"},
        {"agent", "gateway_timeout"},
        {"agent", "tool_use_enforcement"},
        {"terminal", "backend"},
        {"terminal", "docker_image"},
        {"terminal", "persistent_shell"},
        {"browser", "allow_private_urls"},
        {"compression", "enabled"},
        {"compression", "threshold"},
        {"display", "streaming"},
        {"display", "skin"},
        {"display", "show_reasoning"},
        {"smart_model_routing", "enabled"},
        {"privacy", "redact_pii"},
        {"tts", "provider"},
    };
    return paths;
}

std::vector<std::pair<std::string, std::string>> collect_config_overrides(
    const nlohmann::json& config,
    const nlohmann::json& defaults) {
    std::vector<std::pair<std::string, std::string>> out{};

    if (!config.is_object()) {
        return out;
    }

    auto section_or_empty = [](const nlohmann::json& root,
                               const std::string& section) -> const nlohmann::json& {
        static const nlohmann::json empty{nlohmann::json::object()};
        if (!root.is_object()) {
            return empty;
        }
        auto it = root.find(section);
        if (it == root.end() || !it->is_object()) {
            return empty;
        }
        return *it;
    };

    for (const auto& [section, key] : interesting_override_paths()) {
        const nlohmann::json& user_section{section_or_empty(config, section)};
        const nlohmann::json& default_section{section_or_empty(defaults, section)};

        auto user_it = user_section.find(key);
        if (user_it == user_section.end() || user_it->is_null()) {
            continue;
        }
        auto default_it = default_section.find(key);
        if (default_it != default_section.end() && *default_it == *user_it) {
            continue;
        }
        out.emplace_back(section + std::string{"."} + key,
                         python_repr(*user_it));
    }

    // Toolsets
    auto user_toolsets_it = config.find("toolsets");
    if (user_toolsets_it != config.end()) {
        nlohmann::json default_toolsets{nlohmann::json::array()};
        if (defaults.is_object()) {
            auto d_it = defaults.find("toolsets");
            if (d_it != defaults.end()) {
                default_toolsets = *d_it;
            }
        }
        if (*user_toolsets_it != default_toolsets) {
            out.emplace_back(std::string{"toolsets"},
                             python_repr(*user_toolsets_it));
        }
    }

    // Fallback providers
    auto fallbacks_it = config.find("fallback_providers");
    if (fallbacks_it != config.end() && fallbacks_it->is_array() &&
        !fallbacks_it->empty()) {
        out.emplace_back(std::string{"fallback_providers"},
                         python_repr(*fallbacks_it));
    }

    return out;
}

}  // namespace hermes::cli::dump_cmd
