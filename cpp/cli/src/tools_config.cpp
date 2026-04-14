// tools_config — implementation. See tools_config.hpp.
#include "hermes/cli/tools_config.hpp"
#include "hermes/config/loader.hpp"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace hermes::cli::tools_config {

const std::vector<ToolsetInfo>& configurable_toolsets() {
    static const std::vector<ToolsetInfo> kToolsets = {
        {"web",            "Web Search & Scraping",    "web_search, web_extract"},
        {"browser",        "Browser Automation",       "navigate, click, type, scroll"},
        {"terminal",       "Terminal & Processes",     "terminal, process"},
        {"file",           "File Operations",          "read, write, patch, search"},
        {"code_execution", "Code Execution",           "execute_code"},
        {"vision",         "Vision / Image Analysis",  "vision_analyze"},
        {"image_gen",      "Image Generation",         "image_generate"},
        {"moa",            "Mixture of Agents",        "mixture_of_agents"},
        {"tts",            "Text-to-Speech",           "text_to_speech"},
        {"skills",         "Skills",                   "list, view, manage"},
        {"todo",           "Task Planning",            "todo"},
        {"memory",         "Memory",                   "persistent memory across sessions"},
        {"session_search", "Session Search",           "search past conversations"},
        {"clarify",        "Clarifying Questions",     "clarify"},
        {"delegation",     "Task Delegation",          "delegate_task"},
        {"cronjob",        "Cron Jobs",                "create/list/update/pause/resume/run"},
        {"rl",             "RL Training",              "Tinker-Atropos training tools"},
        {"homeassistant",  "Home Assistant",           "smart home device control"},
    };
    return kToolsets;
}

const ToolsetInfo* find_toolset(const std::string& key) {
    for (const auto& t : configurable_toolsets()) {
        if (t.key == key) return &t;
    }
    return nullptr;
}

const std::set<std::string>& default_off_toolsets() {
    static const std::set<std::string> kOff = {"moa", "homeassistant", "rl"};
    return kOff;
}

std::set<std::string> default_enabled_toolsets() {
    std::set<std::string> out;
    const auto& off = default_off_toolsets();
    for (const auto& t : configurable_toolsets()) {
        if (!off.count(t.key)) out.insert(t.key);
    }
    return out;
}

const std::vector<PlatformInfo>& platforms() {
    static const std::vector<PlatformInfo> kPlatforms = {
        {"cli",          "CLI",            "hermes-cli"},
        {"telegram",     "Telegram",       "hermes-telegram"},
        {"discord",      "Discord",        "hermes-discord"},
        {"slack",        "Slack",          "hermes-slack"},
        {"whatsapp",     "WhatsApp",       "hermes-whatsapp"},
        {"signal",       "Signal",         "hermes-signal"},
        {"bluebubbles",  "BlueBubbles",    "hermes-bluebubbles"},
        {"homeassistant","Home Assistant", "hermes-homeassistant"},
        {"email",        "Email",          "hermes-email"},
        {"matrix",       "Matrix",         "hermes-matrix"},
        {"dingtalk",     "DingTalk",       "hermes-dingtalk"},
        {"feishu",       "Feishu",         "hermes-feishu"},
        {"wecom",        "WeCom",          "hermes-wecom"},
        {"weixin",       "Weixin",         "hermes-weixin"},
        {"api_server",   "API Server",     "hermes-api-server"},
        {"mattermost",   "Mattermost",     "hermes-mattermost"},
        {"webhook",      "Webhook",        "hermes-webhook"},
    };
    return kPlatforms;
}

const PlatformInfo* find_platform(const std::string& key) {
    for (const auto& p : platforms()) {
        if (p.key == key) return &p;
    }
    return nullptr;
}

std::set<std::string> get_enabled_toolsets(const nlohmann::json& config,
                                           const std::string& platform) {
    if (config.is_object() && config.contains("platform_toolsets") &&
        config["platform_toolsets"].is_object() &&
        config["platform_toolsets"].contains(platform) &&
        config["platform_toolsets"][platform].is_array()) {
        std::set<std::string> out;
        for (const auto& v : config["platform_toolsets"][platform]) {
            if (v.is_string()) out.insert(v.get<std::string>());
        }
        return out;
    }
    return default_enabled_toolsets();
}

void set_enabled_toolsets(nlohmann::json& config,
                          const std::string& platform,
                          const std::set<std::string>& enabled) {
    if (!config.is_object()) config = nlohmann::json::object();
    if (!config.contains("platform_toolsets") ||
        !config["platform_toolsets"].is_object()) {
        config["platform_toolsets"] = nlohmann::json::object();
    }
    nlohmann::json arr = nlohmann::json::array();
    for (const auto& s : enabled) arr.push_back(s);
    config["platform_toolsets"][platform] = arr;
}

void enable_toolset(nlohmann::json& config, const std::string& platform,
                    const std::string& toolset) {
    auto current = get_enabled_toolsets(config, platform);
    current.insert(toolset);
    set_enabled_toolsets(config, platform, current);
}

void disable_toolset(nlohmann::json& config, const std::string& platform,
                     const std::string& toolset) {
    auto current = get_enabled_toolsets(config, platform);
    current.erase(toolset);
    set_enabled_toolsets(config, platform, current);
}

std::vector<std::string> configured_platforms(const nlohmann::json& config) {
    std::vector<std::string> out;
    if (!config.is_object() || !config.contains("platform_toolsets")) return out;
    const auto& pt = config["platform_toolsets"];
    if (!pt.is_object()) return out;
    for (auto it = pt.begin(); it != pt.end(); ++it) {
        out.push_back(it.key());
    }
    std::sort(out.begin(), out.end());
    return out;
}

// --- Providers --------------------------------------------------------------

const std::vector<ToolCategory>& tool_categories() {
    static const std::vector<ToolCategory> kCategories = {
        {"tts", "Text-to-Speech", {
            {"Nous Subscription", "Managed OpenAI TTS", "openai", {}, true},
            {"Microsoft Edge TTS", "Free - no API key needed", "edge", {}, false},
            {"OpenAI TTS", "Premium quality voices", "openai",
                {"VOICE_TOOLS_OPENAI_KEY"}, false},
            {"ElevenLabs", "Premium - natural voices", "elevenlabs",
                {"ELEVENLABS_API_KEY"}, false},
            {"Mistral (Voxtral)", "Multilingual, native Opus", "mistral",
                {"MISTRAL_API_KEY"}, false},
        }},
        {"web", "Web Search & Extract", {
            {"Nous Subscription", "Managed Firecrawl", "firecrawl", {}, true},
            {"Firecrawl Cloud", "Hosted search+extract", "firecrawl",
                {"FIRECRAWL_API_KEY"}, false},
            {"Exa", "AI-native search", "exa", {"EXA_API_KEY"}, false},
            {"Parallel", "AI-native search+extract", "parallel",
                {"PARALLEL_API_KEY"}, false},
            {"Tavily", "AI-native search+crawl", "tavily",
                {"TAVILY_API_KEY"}, false},
            {"Firecrawl Self-Hosted", "Run your own instance", "firecrawl",
                {"FIRECRAWL_API_URL"}, false},
        }},
        {"image_gen", "Image Generation", {
            {"Nous Subscription", "Managed FAL", "fal", {}, true},
            {"FAL.ai", "FLUX 2 Pro", "fal", {"FAL_KEY"}, false},
        }},
        {"browser", "Browser Automation", {
            {"Nous Subscription", "Managed Browser Use cloud", "browser-use", {}, true},
            {"Local Browser", "Free headless Chromium", "local", {}, false},
            {"Browserbase", "Cloud browser with stealth", "browserbase",
                {"BROWSERBASE_API_KEY", "BROWSERBASE_PROJECT_ID"}, false},
            {"Browser Use", "Cloud browser with remote exec", "browser-use",
                {"BROWSER_USE_API_KEY"}, false},
            {"Firecrawl", "Cloud browser with remote exec", "firecrawl",
                {"FIRECRAWL_API_KEY"}, false},
        }},
        {"vision", "Vision / Image Analysis", {
            {"Nous Subscription", "Managed vision provider", "openai", {}, true},
            {"OpenAI", "GPT-4 Vision", "openai", {"OPENAI_API_KEY"}, false},
            {"Anthropic", "Claude vision", "anthropic",
                {"ANTHROPIC_API_KEY"}, false},
        }},
        {"code_execution", "Code Execution", {
            {"Modal", "Managed sandbox", "modal", {"MODAL_TOKEN_ID"}, false},
            {"Daytona", "Dev sandbox", "daytona", {"DAYTONA_API_KEY"}, false},
            {"Local", "Run locally (danger)", "local", {}, false},
        }},
    };
    return kCategories;
}

const ToolCategory* find_category(const std::string& key) {
    for (const auto& c : tool_categories()) {
        if (c.key == key) return &c;
    }
    return nullptr;
}

std::vector<std::string> env_vars_for_category(const std::string& key) {
    std::vector<std::string> out;
    const auto* cat = find_category(key);
    if (!cat) return out;
    std::set<std::string> seen;
    for (const auto& prov : cat->providers) {
        for (const auto& v : prov.env_vars) {
            if (seen.insert(v).second) out.push_back(v);
        }
    }
    return out;
}

bool env_vars_present(const std::vector<std::string>& vars) {
    for (const auto& v : vars) {
        const char* val = std::getenv(v.c_str());
        if (!val || *val == '\0') return false;
    }
    return !vars.empty();
}

// --- Rendering --------------------------------------------------------------

std::size_t render_toolset_status(std::ostream& out,
                                  const nlohmann::json& config,
                                  const std::string& platform) {
    auto enabled = get_enabled_toolsets(config, platform);
    std::size_t n = 0;
    for (const auto& t : configurable_toolsets()) {
        bool is_on = enabled.count(t.key) > 0;
        out << "  [" << (is_on ? "x" : " ") << "] "
            << t.key << "  —  " << t.description << "\n";
        ++n;
    }
    return n;
}

int cmd_list(std::ostream& out, const nlohmann::json& config) {
    auto plats = configured_platforms(config);
    if (plats.empty()) plats.push_back("cli");
    for (const auto& p : plats) {
        const auto* info = find_platform(p);
        out << "\n" << (info ? info->label : p) << " (" << p << ")\n";
        out << std::string(40, '-') << "\n";
        render_toolset_status(out, config, p);
    }
    out << "\n";
    return 0;
}

// --- dispatch ---------------------------------------------------------------

int dispatch(int argc, char** argv) {
    // hermes tools list              -> per-platform table
    // hermes tools enable <ts> [--platform P]
    // hermes tools disable <ts> [--platform P]
    std::string sub = (argc > 2) ? argv[2] : "list";
    auto config = hermes::config::load_config();
    auto platform_for = [&](int start) -> std::string {
        for (int i = start; i < argc; ++i) {
            std::string a = argv[i];
            if (a == "--platform" && i + 1 < argc) return argv[i + 1];
            if (a.rfind("--platform=", 0) == 0) return a.substr(11);
        }
        return "cli";
    };
    if (sub == "list") return cmd_list(std::cout, config);
    if (sub == "enable" || sub == "disable") {
        if (argc < 4) {
            std::cerr << "Usage: hermes tools " << sub
                      << " <toolset> [--platform <p>]\n";
            return 1;
        }
        std::string ts = argv[3];
        std::string platform = platform_for(4);
        if (!find_toolset(ts)) {
            std::cerr << "Unknown toolset: " << ts << "\n";
            return 1;
        }
        if (sub == "enable") enable_toolset(config, platform, ts);
        else disable_toolset(config, platform, ts);
        hermes::config::save_config(config);
        std::cout << (sub == "enable" ? "Enabled " : "Disabled ")
                  << ts << " for " << platform << "\n";
        return 0;
    }
    std::cerr << "Unknown tools subcommand: " << sub << "\n";
    return 1;
}

}  // namespace hermes::cli::tools_config
