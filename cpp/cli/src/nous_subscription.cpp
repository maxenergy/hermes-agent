// C++17 port of the pure-logic helpers from
// `hermes_cli/nous_subscription.py`.  See the header for the scope of
// the port.

#include "hermes/cli/nous_subscription.hpp"

#include <algorithm>
#include <cctype>
#include <stdexcept>
#include <unordered_map>

namespace hermes::cli::nous_subscription {
namespace {

std::string to_lower(const std::string& value) {
    std::string out{value};
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

std::string trim(const std::string& value) {
    auto is_space = [](unsigned char c) { return std::isspace(c) != 0; };
    auto first = std::find_if_not(value.begin(), value.end(), is_space);
    auto last = std::find_if_not(value.rbegin(), value.rend(), is_space).base();
    if (first >= last) {
        return std::string{};
    }
    return std::string{first, last};
}

}  // namespace

const nous_feature_state& nous_subscription_features::at(
    const std::string& key) const {
    auto it = features.find(key);
    if (it == features.end()) {
        throw std::out_of_range{"nous feature key not present: " + key};
    }
    return it->second;
}

std::vector<nous_feature_state> nous_subscription_features::ordered_items() const {
    std::vector<nous_feature_state> out{};
    out.reserve(k_feature_order.size());
    for (const char* key : k_feature_order) {
        auto it = features.find(key);
        if (it != features.end()) {
            out.push_back(it->second);
        }
    }
    return out;
}

nlohmann::json model_config_dict(const nlohmann::json& config) {
    if (!config.is_object()) {
        return nlohmann::json::object();
    }
    auto it = config.find("model");
    if (it == config.end()) {
        return nlohmann::json::object();
    }
    if (it->is_object()) {
        return *it;  // copy the existing object verbatim
    }
    if (it->is_string()) {
        std::string raw{it->get<std::string>()};
        std::string trimmed{trim(raw)};
        if (!trimmed.empty()) {
            nlohmann::json out = nlohmann::json::object();
            out["default"] = trimmed;
            return out;
        }
    }
    return nlohmann::json::object();
}

std::string browser_label(const std::string& current_provider) {
    static const std::unordered_map<std::string, std::string> mapping{
        {"browserbase", "Browserbase"},
        {"browser-use", "Browser Use"},
        {"firecrawl", "Firecrawl"},
        {"camofox", "Camofox"},
        {"local", "Local browser"},
    };

    std::string key{current_provider.empty() ? std::string{"local"} : current_provider};
    auto it = mapping.find(key);
    if (it != mapping.end()) {
        return it->second;
    }
    return current_provider.empty() ? std::string{"Local browser"} : current_provider;
}

std::string tts_label(const std::string& current_provider) {
    static const std::unordered_map<std::string, std::string> mapping{
        {"openai", "OpenAI TTS"},
        {"elevenlabs", "ElevenLabs"},
        {"edge", "Edge TTS"},
        {"mistral", "Mistral Voxtral TTS"},
        {"neutts", "NeuTTS"},
    };

    std::string key{current_provider.empty() ? std::string{"edge"} : current_provider};
    auto it = mapping.find(key);
    if (it != mapping.end()) {
        return it->second;
    }
    return current_provider.empty() ? std::string{"Edge TTS"} : current_provider;
}

browser_feature_decision resolve_browser_feature_state(
    const browser_feature_inputs& inputs) {
    browser_feature_decision out{};

    if (inputs.direct_camofox) {
        out.current_provider = "camofox";
        out.available = true;
        out.active = inputs.browser_tool_enabled;
        out.managed = false;
        return out;
    }

    if (inputs.browser_provider_explicit) {
        std::string current_provider{inputs.browser_provider.empty()
                                         ? std::string{"local"}
                                         : inputs.browser_provider};
        if (current_provider == "browserbase") {
            out.current_provider = current_provider;
            out.available = inputs.browser_local_available && inputs.direct_browserbase;
            out.active = inputs.browser_tool_enabled && out.available;
            out.managed = false;
            return out;
        }
        if (current_provider == "browser-use") {
            bool provider_available{inputs.managed_browser_available ||
                                     inputs.direct_browser_use};
            out.current_provider = current_provider;
            out.available = inputs.browser_local_available && provider_available;
            out.managed = inputs.browser_tool_enabled &&
                          inputs.browser_local_available &&
                          inputs.managed_browser_available &&
                          !inputs.direct_browser_use;
            out.active = inputs.browser_tool_enabled && out.available;
            return out;
        }
        if (current_provider == "firecrawl") {
            out.current_provider = current_provider;
            out.available = inputs.browser_local_available && inputs.direct_firecrawl;
            out.active = inputs.browser_tool_enabled && out.available;
            out.managed = false;
            return out;
        }
        if (current_provider == "camofox") {
            out.current_provider = current_provider;
            out.available = false;
            out.active = false;
            out.managed = false;
            return out;
        }

        // Fallback -- unrecognised explicit value becomes "local".
        out.current_provider = "local";
        out.available = inputs.browser_local_available;
        out.active = inputs.browser_tool_enabled && out.available;
        out.managed = false;
        return out;
    }

    if (inputs.managed_browser_available || inputs.direct_browser_use) {
        out.current_provider = "browser-use";
        out.available = inputs.browser_local_available;
        out.managed = inputs.browser_tool_enabled &&
                      inputs.browser_local_available &&
                      inputs.managed_browser_available &&
                      !inputs.direct_browser_use;
        out.active = inputs.browser_tool_enabled && out.available;
        return out;
    }

    if (inputs.direct_browserbase) {
        out.current_provider = "browserbase";
        out.available = inputs.browser_local_available;
        out.active = inputs.browser_tool_enabled && out.available;
        out.managed = false;
        return out;
    }

    out.current_provider = "local";
    out.available = inputs.browser_local_available;
    out.active = inputs.browser_tool_enabled && out.available;
    out.managed = false;
    return out;
}

std::vector<std::string> nous_subscription_explainer_lines(bool managed_enabled) {
    if (!managed_enabled) {
        return {};
    }
    return {
        std::string{"Nous subscription enables managed web tools, image "
                    "generation, OpenAI TTS, and browser automation by default."},
        std::string{"Those managed tools bill to your Nous subscription. Modal "
                    "execution is optional and can bill to your subscription too."},
        std::string{"Change these later with: hermes setup tools, hermes setup "
                    "terminal, or hermes status."},
    };
}

managed_defaults_decision apply_nous_managed_defaults_decision(
    const managed_defaults_inputs& inputs) {
    managed_defaults_decision out{};

    if (!inputs.managed_enabled || !inputs.provider_is_nous) {
        return out;
    }

    const auto has = [&](const std::string& key) {
        return inputs.selected_toolsets.count(key) > 0;
    };

    if (has("web") && !inputs.web_explicit_configured &&
        !inputs.has_parallel_or_tavily_or_firecrawl_env) {
        out.set_web_backend_firecrawl = true;
        out.to_change.insert("web");
    }

    if (has("tts") && !inputs.tts_explicit_configured &&
        !inputs.has_openai_audio_or_elevenlabs_env) {
        out.set_tts_provider_openai = true;
        out.to_change.insert("tts");
    }

    if (has("browser") && !inputs.browser_explicit_configured &&
        !inputs.has_browser_use_or_browserbase_env) {
        out.set_browser_cloud_provider_browser_use = true;
        out.to_change.insert("browser");
    }

    if (has("image_gen") && !inputs.has_fal_env) {
        out.to_change.insert("image_gen");
    }

    return out;
}

provider_defaults_decision apply_nous_provider_defaults_decision(
    bool managed_enabled,
    bool provider_is_nous,
    const std::string& current_tts_provider) {
    provider_defaults_decision out{};
    if (!managed_enabled || !provider_is_nous) {
        return out;
    }

    std::string normalized{to_lower(trim(current_tts_provider))};
    if (normalized.empty() || normalized == "edge") {
        out.set_tts_provider_openai = true;
        out.to_change.insert("tts");
    }
    return out;
}

}  // namespace hermes::cli::nous_subscription
