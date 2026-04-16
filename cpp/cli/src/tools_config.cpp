// tools_config — implementation. See tools_config.hpp.
#include "hermes/cli/tools_config.hpp"
#include "hermes/cli/colors.hpp"
#include "hermes/cli/curses_ui.hpp"
#include "hermes/config/loader.hpp"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace hermes::cli::tools_config {

namespace col = hermes::cli::colors;
namespace fs = std::filesystem;

// ===========================================================================
// Toolset registry
// ===========================================================================

const std::vector<ToolsetInfo>& configurable_toolsets() {
    static const std::vector<ToolsetInfo> kToolsets = {
        {"web",            "Web Search & Scraping",    "web_search, web_extract", 20.0,
            {"web_search", "web_extract", "browser_navigate"}},
        {"browser",        "Browser Automation",       "navigate, click, type, scroll", 30.0,
            {"browser_navigate", "browser_click", "browser_type",
             "browser_scroll", "browser_screenshot"}},
        {"terminal",       "Terminal & Processes",     "terminal, process", 0.0,
            {"terminal", "process_start", "process_list", "process_kill"}},
        {"file",           "File Operations",          "read, write, patch, search", 0.0,
            {"file_read", "file_write", "file_patch", "file_search"}},
        {"code_execution", "Code Execution",           "execute_code", 15.0,
            {"execute_code"}},
        {"vision",         "Vision / Image Analysis",  "vision_analyze", 5.0,
            {"vision_analyze"}},
        {"image_gen",      "Image Generation",         "image_generate", 25.0,
            {"image_generate"}},
        {"moa",            "Mixture of Agents",        "mixture_of_agents", 50.0,
            {"mixture_of_agents"}},
        {"tts",            "Text-to-Speech",           "text_to_speech", 10.0,
            {"text_to_speech"}},
        {"skills",         "Skills",                   "list, view, manage", 0.0,
            {"skill_list", "skill_view", "skill_install"}},
        {"todo",           "Task Planning",            "todo", 0.0,
            {"todo"}},
        {"memory",         "Memory",                   "persistent memory across sessions", 0.0,
            {"memory_write", "memory_read", "memory_search", "memory_delete"}},
        {"session_search", "Session Search",           "search past conversations", 0.0,
            {"session_search"}},
        {"clarify",        "Clarifying Questions",     "clarify", 0.0,
            {"clarify"}},
        {"delegation",     "Task Delegation",          "delegate_task", 5.0,
            {"delegate_task"}},
        {"cronjob",        "Cron Jobs",                "create/list/update/pause/resume/run", 0.0,
            {"cron_create", "cron_list", "cron_update", "cron_pause",
             "cron_resume", "cron_run"}},
        {"rl",             "RL Training",              "Tinker-Atropos training tools", 0.0,
            {"rl_train", "rl_rollout", "rl_evaluate"}},
        {"homeassistant",  "Home Assistant",           "smart home device control", 0.0,
            {"ha_call_service", "ha_get_state", "ha_list_entities"}},
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
        {"cli",          "CLI",            "hermes-cli",          ""},
        {"telegram",     "Telegram",       "hermes-telegram",     "TELEGRAM_BOT_TOKEN"},
        {"discord",      "Discord",        "hermes-discord",      "DISCORD_BOT_TOKEN"},
        {"slack",        "Slack",          "hermes-slack",        "SLACK_BOT_TOKEN"},
        {"whatsapp",     "WhatsApp",       "hermes-whatsapp",     "WHATSAPP_ACCESS_TOKEN"},
        {"signal",       "Signal",         "hermes-signal",       "SIGNAL_CLI_PATH"},
        {"bluebubbles",  "BlueBubbles",    "hermes-bluebubbles",  "BLUEBUBBLES_SERVER_URL"},
        {"homeassistant","Home Assistant", "hermes-homeassistant","HOMEASSISTANT_TOKEN"},
        {"email",        "Email",          "hermes-email",        "HERMES_EMAIL_IMAP_USER"},
        {"matrix",       "Matrix",         "hermes-matrix",       "MATRIX_ACCESS_TOKEN"},
        {"dingtalk",     "DingTalk",       "hermes-dingtalk",     "DINGTALK_APP_KEY"},
        {"feishu",       "Feishu",         "hermes-feishu",       "FEISHU_APP_ID"},
        {"wecom",        "WeCom",          "hermes-wecom",        "WECOM_CORP_ID"},
        {"weixin",       "Weixin",         "hermes-weixin",       "WEIXIN_APP_ID"},
        {"api_server",   "API Server",     "hermes-api-server",   "API_SERVER_BIND"},
        {"mattermost",   "Mattermost",     "hermes-mattermost",   "MATTERMOST_TOKEN"},
        {"webhook",      "Webhook",        "hermes-webhook",      "WEBHOOK_SECRET"},
    };
    return kPlatforms;
}

const PlatformInfo* find_platform(const std::string& key) {
    for (const auto& p : platforms()) {
        if (p.key == key) return &p;
    }
    return nullptr;
}

std::vector<std::string> detected_platforms() {
    std::vector<std::string> out;
    for (const auto& p : platforms()) {
        if (p.probe_env_var.empty()) continue;
        const char* v = std::getenv(p.probe_env_var.c_str());
        if (v && *v) out.push_back(p.key);
    }
    return out;
}

// ===========================================================================
// Config read / write
// ===========================================================================

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

bool write_json_atomic(const std::string& path, const nlohmann::json& config) {
    fs::path target(path);
    std::error_code ec;
    fs::create_directories(target.parent_path(), ec);
    auto tmp = target.parent_path() / (target.filename().string() + ".tmp");
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) return false;
        f << config.dump(2);
        if (!f) return false;
    }
    fs::rename(tmp, target, ec);
    if (ec) {
        fs::remove(tmp, ec);
        return false;
    }
    return true;
}

bool save_tools_config_atomic(const nlohmann::json& config) {
    // The Python layer is YAML-backed; we re-use the config loader's
    // save_config which handles the YAML encoding.
    try {
        hermes::config::save_config(config);
        return true;
    } catch (...) {
        return false;
    }
}

// ===========================================================================
// Allow/Deny
// ===========================================================================

AllowDeny read_tool_acl(const nlohmann::json& config,
                        const std::string& platform) {
    AllowDeny out;
    if (!config.is_object() || !config.contains("tool_acl")) return out;
    const auto& acl = config["tool_acl"];
    if (!acl.is_object() || !acl.contains(platform)) return out;
    const auto& pl = acl[platform];
    if (!pl.is_object()) return out;
    if (pl.contains("allow") && pl["allow"].is_array()) {
        for (const auto& v : pl["allow"]) {
            if (v.is_string()) out.allow.insert(v.get<std::string>());
        }
    }
    if (pl.contains("deny") && pl["deny"].is_array()) {
        for (const auto& v : pl["deny"]) {
            if (v.is_string()) out.deny.insert(v.get<std::string>());
        }
    }
    return out;
}

void write_tool_acl(nlohmann::json& config,
                    const std::string& platform,
                    const AllowDeny& acl) {
    if (!config.is_object()) config = nlohmann::json::object();
    if (!config.contains("tool_acl") || !config["tool_acl"].is_object()) {
        config["tool_acl"] = nlohmann::json::object();
    }
    nlohmann::json pl = nlohmann::json::object();
    nlohmann::json a = nlohmann::json::array();
    for (const auto& x : acl.allow) a.push_back(x);
    nlohmann::json d = nlohmann::json::array();
    for (const auto& x : acl.deny) d.push_back(x);
    pl["allow"] = a;
    pl["deny"] = d;
    config["tool_acl"][platform] = pl;
}

bool is_tool_allowed(const AllowDeny& acl, const std::string& tool) {
    if (acl.deny.count(tool)) return false;
    if (!acl.allow.empty() && !acl.allow.count(tool)) return false;
    return true;
}

std::vector<std::string> all_tools() {
    std::vector<std::string> out;
    std::set<std::string> seen;
    for (const auto& ts : configurable_toolsets()) {
        for (const auto& t : ts.tools) {
            if (seen.insert(t).second) out.push_back(t);
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}

// ===========================================================================
// Providers
// ===========================================================================

const std::vector<ToolCategory>& tool_categories() {
    static const std::vector<ToolCategory> kCategories = {
        {"tts", "Text-to-Speech", {
            {"Nous Subscription", "Managed OpenAI TTS", "openai", {}, true,  0.0},
            {"Microsoft Edge TTS","Free - no API key needed", "edge", {}, false, 0.0},
            {"OpenAI TTS", "Premium quality voices", "openai",
                {"VOICE_TOOLS_OPENAI_KEY"}, false, 0.015},
            {"ElevenLabs", "Premium - natural voices", "elevenlabs",
                {"ELEVENLABS_API_KEY"}, false, 0.030},
            {"Mistral (Voxtral)", "Multilingual, native Opus", "mistral",
                {"MISTRAL_API_KEY"}, false, 0.010},
        }},
        {"web", "Web Search & Extract", {
            {"Nous Subscription", "Managed Firecrawl", "firecrawl", {}, true, 0.0},
            {"Firecrawl Cloud", "Hosted search+extract", "firecrawl",
                {"FIRECRAWL_API_KEY"}, false, 0.015},
            {"Exa", "AI-native search", "exa", {"EXA_API_KEY"}, false, 0.005},
            {"Parallel", "AI-native search+extract", "parallel",
                {"PARALLEL_API_KEY"}, false, 0.010},
            {"Tavily", "AI-native search+crawl", "tavily",
                {"TAVILY_API_KEY"}, false, 0.008},
            {"Firecrawl Self-Hosted", "Run your own instance", "firecrawl",
                {"FIRECRAWL_API_URL"}, false, 0.0},
        }},
        {"image_gen", "Image Generation", {
            {"Nous Subscription", "Managed FAL", "fal", {}, true, 0.0},
            {"FAL.ai", "FLUX 2 Pro", "fal", {"FAL_KEY"}, false, 0.040},
        }},
        {"browser", "Browser Automation", {
            {"Nous Subscription", "Managed Browser Use cloud", "browser-use", {}, true, 0.0},
            {"Local Browser", "Free headless Chromium", "local", {}, false, 0.0},
            {"Browserbase", "Cloud browser with stealth", "browserbase",
                {"BROWSERBASE_API_KEY", "BROWSERBASE_PROJECT_ID"}, false, 0.050},
            {"Browser Use", "Cloud browser with remote exec", "browser-use",
                {"BROWSER_USE_API_KEY"}, false, 0.050},
            {"Firecrawl", "Cloud browser with remote exec", "firecrawl",
                {"FIRECRAWL_API_KEY"}, false, 0.015},
        }},
        {"vision", "Vision / Image Analysis", {
            {"Nous Subscription", "Managed vision provider", "openai", {}, true, 0.0},
            {"OpenAI", "GPT-4 Vision", "openai", {"OPENAI_API_KEY"}, false, 0.005},
            {"Anthropic", "Claude vision", "anthropic",
                {"ANTHROPIC_API_KEY"}, false, 0.010},
        }},
        {"code_execution", "Code Execution", {
            {"Modal", "Managed sandbox", "modal", {"MODAL_TOKEN_ID"}, false, 0.003},
            {"Daytona", "Dev sandbox", "daytona", {"DAYTONA_API_KEY"}, false, 0.002},
            {"Local", "Run locally (danger)", "local", {}, false, 0.0},
        }},
        {"memory", "Memory Store", {
            {"Chitta (local)", "Local Rust memory daemon", "chitta", {}, false, 0.0},
            {"SQLite", "Flat local SQLite store", "sqlite", {}, false, 0.0},
            {"Pinecone", "Managed vector DB", "pinecone",
                {"PINECONE_API_KEY"}, false, 0.001},
            {"Qdrant", "Self-hosted vector DB", "qdrant",
                {"QDRANT_URL"}, false, 0.0},
        }},
        {"session_search", "Session Search", {
            {"Local FTS5", "SQLite FTS5 over sessions", "local", {}, false, 0.0},
            {"Elastic", "Elasticsearch cluster", "elastic",
                {"ELASTIC_URL", "ELASTIC_API_KEY"}, false, 0.0},
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

const ProviderOption* resolve_active_provider(const ToolCategory& cat) {
    for (const auto& p : cat.providers) {
        if (p.env_vars.empty()) continue;
        if (env_vars_present(p.env_vars)) return &p;
    }
    return nullptr;
}

std::string active_backend_for_category(const nlohmann::json& config,
                                        const std::string& category_key) {
    // 1) honour config["providers"][category] == backend
    if (config.is_object() && config.contains("providers") &&
        config["providers"].is_object() &&
        config["providers"].contains(category_key) &&
        config["providers"][category_key].is_string()) {
        return config["providers"][category_key].get<std::string>();
    }
    // 2) fall back to first provider with env set
    const auto* cat = find_category(category_key);
    if (!cat) return "";
    const auto* active = resolve_active_provider(*cat);
    if (active) return active->backend;
    // 3) fallback: first free/no-key option.
    for (const auto& p : cat->providers) {
        if (p.env_vars.empty() && !p.requires_nous_auth) return p.backend;
    }
    return "";
}

// ===========================================================================
// Cost preview
// ===========================================================================

CostEstimate estimate_monthly_cost(const std::set<std::string>& enabled) {
    CostEstimate out;
    for (const auto& ts : configurable_toolsets()) {
        if (!enabled.count(ts.key)) continue;
        if (ts.est_monthly_usd <= 0.0) continue;
        out.monthly_usd += ts.est_monthly_usd;
        out.breakdown.emplace_back(ts.key, ts.est_monthly_usd);
    }
    std::sort(out.breakdown.begin(), out.breakdown.end(),
              [](const auto& a, const auto& b) { return a.second > b.second; });
    return out;
}

void render_cost_estimate(std::ostream& out, const CostEstimate& est) {
    out << "\nEstimated monthly cost: $" << std::fixed << std::setprecision(2)
        << est.monthly_usd << " USD\n";
    out << std::string(40, '-') << "\n";
    for (const auto& [k, v] : est.breakdown) {
        out << "  " << std::left << std::setw(20) << k
            << "  $" << std::fixed << std::setprecision(2) << v << "\n";
    }
    if (est.breakdown.empty()) {
        out << "  (no paid toolsets enabled)\n";
    }
}

// ===========================================================================
// Rendering (non-interactive)
// ===========================================================================

std::size_t render_toolset_status(std::ostream& out,
                                  const nlohmann::json& config,
                                  const std::string& platform) {
    auto enabled = get_enabled_toolsets(config, platform);
    std::size_t n = 0;
    for (const auto& t : configurable_toolsets()) {
        bool is_on = enabled.count(t.key) > 0;
        out << "  [" << (is_on ? "x" : " ") << "] "
            << std::left << std::setw(18) << t.key << "  "
            << t.description;
        if (t.est_monthly_usd > 0.0) {
            out << "   (~$" << std::fixed << std::setprecision(0)
                << t.est_monthly_usd << "/mo)";
        }
        out << "\n";
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
        out << std::string(60, '-') << "\n";
        render_toolset_status(out, config, p);
    }
    out << "\n";
    return 0;
}

void render_provider_table(std::ostream& out, const nlohmann::json& config) {
    out << "\nActive providers\n";
    out << std::string(60, '-') << "\n";
    for (const auto& c : tool_categories()) {
        std::string backend = active_backend_for_category(config, c.key);
        out << "  " << std::left << std::setw(20) << c.key
            << "  " << std::setw(20) << c.name
            << "  " << (backend.empty() ? "(none)" : backend) << "\n";
    }
    out << "\n";
}

void render_acl(std::ostream& out,
                const nlohmann::json& config,
                const std::string& platform) {
    auto acl = read_tool_acl(config, platform);
    out << "\nTool ACL for " << platform << "\n";
    out << std::string(40, '-') << "\n";
    out << "  allow: ";
    if (acl.allow.empty()) {
        out << "(unrestricted — all tools allowed)\n";
    } else {
        out << "\n";
        for (const auto& t : acl.allow) out << "    + " << t << "\n";
    }
    out << "  deny:  ";
    if (acl.deny.empty()) {
        out << "(none)\n";
    } else {
        out << "\n";
        for (const auto& t : acl.deny) out << "    - " << t << "\n";
    }
    out << "\n";
}

// ===========================================================================
// Interactive editor
// ===========================================================================

EditorState make_editor(const nlohmann::json& config,
                        const std::string& platform) {
    EditorState s;
    s.platform = platform;
    s.enabled = get_enabled_toolsets(config, platform);
    s.acl = read_tool_acl(config, platform);
    s.cursor = 0;
    return s;
}

static std::vector<std::string> toolset_keys() {
    std::vector<std::string> out;
    for (const auto& t : configurable_toolsets()) out.push_back(t.key);
    return out;
}

bool apply_key(EditorState& state, const std::string& key) {
    const auto keys = toolset_keys();
    const std::size_t n = keys.size();
    std::size_t prev_cursor = state.cursor;
    if (key == "UP" || key == "k") {
        if (state.cursor > 0) --state.cursor;
    } else if (key == "DOWN" || key == "j") {
        if (state.cursor + 1 < n) ++state.cursor;
    } else if (key == "HOME" || key == "g") {
        state.cursor = 0;
    } else if (key == "END" || key == "G") {
        state.cursor = n > 0 ? n - 1 : 0;
    } else if (key == "PGUP") {
        state.cursor = state.cursor >= 10 ? state.cursor - 10 : 0;
    } else if (key == "PGDN") {
        state.cursor = state.cursor + 10 < n ? state.cursor + 10 : (n ? n - 1 : 0);
    } else if (key == "SPACE" || key == " ") {
        if (state.view == EditorView::ToolsetList && state.cursor < n) {
            const auto& k = keys[state.cursor];
            if (state.enabled.count(k)) state.enabled.erase(k);
            else state.enabled.insert(k);
            state.dirty = true;
        }
    } else if (key == "a" && state.view == EditorView::ToolsetList) {
        // Enable all.
        for (const auto& k : keys) state.enabled.insert(k);
        state.dirty = true;
    } else if (key == "n" && state.view == EditorView::ToolsetList) {
        // Disable all.
        state.enabled.clear();
        state.dirty = true;
    } else if (key == "d" && state.view == EditorView::ToolsetList) {
        // Restore defaults.
        state.enabled = default_enabled_toolsets();
        state.dirty = true;
    } else if (key == "c" || key == "$") {
        state.view = EditorView::CostPreview;
    } else if (key == "p") {
        if (state.view == EditorView::ToolsetList && state.cursor < n) {
            state.current_category = keys[state.cursor];
            state.view = EditorView::ProviderList;
        }
    } else if (key == "x") {
        state.view = EditorView::AclEditor;
    } else if (key == "ENTER") {
        if (state.view != EditorView::ToolsetList) {
            state.view = EditorView::ToolsetList;
        } else {
            state.done = true;
        }
    } else if (key == "ESC" || key == "q") {
        if (state.view != EditorView::ToolsetList) {
            state.view = EditorView::ToolsetList;
        } else {
            state.cancelled = true;
            state.done = true;
        }
    } else {
        return false;
    }
    return state.cursor != prev_cursor || state.dirty ||
           state.view != EditorView::ToolsetList || state.done;
}

void apply_to_config(const EditorState& state, nlohmann::json& config) {
    set_enabled_toolsets(config, state.platform, state.enabled);
    write_tool_acl(config, state.platform, state.acl);
}

static std::string pad(const std::string& s, std::size_t w) {
    if (s.size() >= w) return s.substr(0, w);
    return s + std::string(w - s.size(), ' ');
}

std::vector<std::string> render_editor(const EditorState& state,
                                       std::size_t width) {
    std::vector<std::string> lines;
    if (state.view == EditorView::ToolsetList) {
        lines.push_back(pad("Toolsets (" + state.platform + ")", width));
        lines.push_back(std::string(width, '-'));
        std::size_t idx = 0;
        for (const auto& t : configurable_toolsets()) {
            bool on = state.enabled.count(t.key) > 0;
            std::ostringstream os;
            os << (idx == state.cursor ? "> " : "  ")
               << "[" << (on ? "x" : " ") << "] "
               << pad(t.key, 18) << "  " << t.description;
            lines.push_back(pad(os.str(), width));
            ++idx;
        }
        lines.push_back(std::string(width, '-'));
        lines.push_back(pad("space: toggle  a: all  n: none  d: defaults  "
                             "c: cost  p: providers  x: acl  q: quit", width));
    } else if (state.view == EditorView::CostPreview) {
        auto est = estimate_monthly_cost(state.enabled);
        std::ostringstream os;
        os << "Est. monthly cost: $" << std::fixed << std::setprecision(2)
           << est.monthly_usd << " USD";
        lines.push_back(pad(os.str(), width));
        lines.push_back(std::string(width, '-'));
        for (const auto& [k, v] : est.breakdown) {
            std::ostringstream row;
            row << "  " << pad(k, 20) << "$" << std::fixed
                << std::setprecision(2) << v;
            lines.push_back(pad(row.str(), width));
        }
        lines.push_back(pad("(enter/esc to return)", width));
    } else if (state.view == EditorView::ProviderList) {
        lines.push_back(pad("Providers for " + state.current_category, width));
        lines.push_back(std::string(width, '-'));
        const auto* cat = find_category(state.current_category);
        if (cat) {
            for (const auto& p : cat->providers) {
                std::ostringstream os;
                os << "  " << pad(p.name, 24)
                   << "[" << p.backend << "] "
                   << p.tag;
                lines.push_back(pad(os.str(), width));
            }
        } else {
            lines.push_back(pad("  (no provider catalog for this toolset)",
                                 width));
        }
        lines.push_back(pad("(enter/esc to return)", width));
    } else if (state.view == EditorView::AclEditor) {
        lines.push_back(pad("Tool ACL — allow / deny", width));
        lines.push_back(std::string(width, '-'));
        for (const auto& a : state.acl.allow) {
            lines.push_back(pad("  + " + a, width));
        }
        for (const auto& d : state.acl.deny) {
            lines.push_back(pad("  - " + d, width));
        }
        lines.push_back(pad("(enter/esc to return)", width));
    }
    return lines;
}

// ===========================================================================
// CLI entry point
// ===========================================================================

namespace {

std::string next_arg(int argc, char** argv, int& i, bool& ok) {
    if (i + 1 >= argc) { ok = false; return ""; }
    ok = true;
    return argv[++i];
}

std::string platform_from_args(int argc, char** argv, int start_idx,
                                const std::string& fallback = "cli") {
    for (int i = start_idx; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--platform" && i + 1 < argc) return argv[i + 1];
        if (a.rfind("--platform=", 0) == 0) return a.substr(11);
    }
    return fallback;
}

void print_help() {
    std::cout
        << "hermes tools — configure agent toolsets\n\n"
        << "Usage:\n"
        << "  hermes tools list [--platform P]\n"
        << "  hermes tools enable <toolset> [--platform P]\n"
        << "  hermes tools disable <toolset> [--platform P]\n"
        << "  hermes tools reset [--platform P]\n"
        << "  hermes tools allow <tool> [--platform P]\n"
        << "  hermes tools deny  <tool> [--platform P]\n"
        << "  hermes tools cost  [--platform P]\n"
        << "  hermes tools providers\n"
        << "  hermes tools edit  [--platform P]\n";
}

}  // namespace

int cmd_list_cli(int argc, char** argv) {
    auto config = hermes::config::load_config();
    std::string platform = platform_from_args(argc, argv, 3, "");
    if (!platform.empty()) {
        if (!find_platform(platform)) {
            std::cerr << "Unknown platform: " << platform << "\n";
            return 1;
        }
        const auto* info = find_platform(platform);
        std::cout << "\n" << info->label << " (" << platform << ")\n";
        std::cout << std::string(60, '-') << "\n";
        render_toolset_status(std::cout, config, platform);
        std::cout << "\n";
        return 0;
    }
    return cmd_list(std::cout, config);
}

int cmd_enable_cli(int argc, char** argv) {
    if (argc < 4) {
        std::cerr << "Usage: hermes tools enable <toolset> [--platform P]\n";
        return 1;
    }
    std::string ts = argv[3];
    std::string platform = platform_from_args(argc, argv, 4);
    if (!find_toolset(ts)) {
        std::cerr << "Unknown toolset: " << ts << "\n";
        return 1;
    }
    auto config = hermes::config::load_config();
    enable_toolset(config, platform, ts);
    if (!save_tools_config_atomic(config)) {
        std::cerr << "Failed to save config\n";
        return 1;
    }
    std::cout << "Enabled " << ts << " for " << platform << "\n";
    return 0;
}

int cmd_disable_cli(int argc, char** argv) {
    if (argc < 4) {
        std::cerr << "Usage: hermes tools disable <toolset> [--platform P]\n";
        return 1;
    }
    std::string ts = argv[3];
    std::string platform = platform_from_args(argc, argv, 4);
    if (!find_toolset(ts)) {
        std::cerr << "Unknown toolset: " << ts << "\n";
        return 1;
    }
    auto config = hermes::config::load_config();
    disable_toolset(config, platform, ts);
    if (!save_tools_config_atomic(config)) {
        std::cerr << "Failed to save config\n";
        return 1;
    }
    std::cout << "Disabled " << ts << " for " << platform << "\n";
    return 0;
}

int cmd_reset_cli(int argc, char** argv) {
    std::string platform = platform_from_args(argc, argv, 3);
    auto config = hermes::config::load_config();
    set_enabled_toolsets(config, platform, default_enabled_toolsets());
    if (!save_tools_config_atomic(config)) return 1;
    std::cout << "Reset toolsets for " << platform << " to defaults.\n";
    return 0;
}

int cmd_allow_cli(int argc, char** argv) {
    if (argc < 4) {
        std::cerr << "Usage: hermes tools allow <tool> [--platform P]\n";
        return 1;
    }
    std::string tool = argv[3];
    std::string platform = platform_from_args(argc, argv, 4);
    auto config = hermes::config::load_config();
    auto acl = read_tool_acl(config, platform);
    acl.allow.insert(tool);
    acl.deny.erase(tool);
    write_tool_acl(config, platform, acl);
    if (!save_tools_config_atomic(config)) return 1;
    std::cout << "Allowed " << tool << " on " << platform << "\n";
    return 0;
}

int cmd_deny_cli(int argc, char** argv) {
    if (argc < 4) {
        std::cerr << "Usage: hermes tools deny <tool> [--platform P]\n";
        return 1;
    }
    std::string tool = argv[3];
    std::string platform = platform_from_args(argc, argv, 4);
    auto config = hermes::config::load_config();
    auto acl = read_tool_acl(config, platform);
    acl.deny.insert(tool);
    acl.allow.erase(tool);
    write_tool_acl(config, platform, acl);
    if (!save_tools_config_atomic(config)) return 1;
    std::cout << "Denied " << tool << " on " << platform << "\n";
    return 0;
}

int cmd_cost_cli(int argc, char** argv) {
    std::string platform = platform_from_args(argc, argv, 3);
    auto config = hermes::config::load_config();
    auto enabled = get_enabled_toolsets(config, platform);
    auto est = estimate_monthly_cost(enabled);
    render_cost_estimate(std::cout, est);
    return 0;
}

int cmd_providers_cli(int /*argc*/, char** /*argv*/) {
    auto config = hermes::config::load_config();
    render_provider_table(std::cout, config);
    return 0;
}

int cmd_edit_cli(int argc, char** argv) {
    // Non-TTY fallback: render the editor layout as flat text.  The
    // interactive curses UI lives in curses_ui and is the default when a
    // TTY is attached; this path exercises the same state machine for
    // scripts and unit tests.
    std::string platform = platform_from_args(argc, argv, 3);
    auto config = hermes::config::load_config();
    auto state = make_editor(config, platform);
    auto lines = render_editor(state);
    for (const auto& l : lines) std::cout << l << "\n";
    return 0;
}

int dispatch(int argc, char** argv) {
    std::string sub = (argc > 2) ? argv[2] : "list";
    if (sub == "--help" || sub == "-h" || sub == "help") {
        print_help();
        return 0;
    }
    if (sub == "list")      return cmd_list_cli(argc, argv);
    if (sub == "enable")    return cmd_enable_cli(argc, argv);
    if (sub == "disable")   return cmd_disable_cli(argc, argv);
    if (sub == "reset")     return cmd_reset_cli(argc, argv);
    if (sub == "allow")     return cmd_allow_cli(argc, argv);
    if (sub == "deny")      return cmd_deny_cli(argc, argv);
    if (sub == "cost")      return cmd_cost_cli(argc, argv);
    if (sub == "providers") return cmd_providers_cli(argc, argv);
    if (sub == "edit")      return cmd_edit_cli(argc, argv);
    std::cerr << "Unknown tools subcommand: " << sub << "\n";
    print_help();
    return 1;
}

}  // namespace hermes::cli::tools_config
