// status — implementation. See status.hpp.
#include "hermes/cli/status.hpp"
#include "hermes/config/loader.hpp"
#include "hermes/core/path.hpp"

#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <system_error>

namespace hermes::cli::status {

namespace fs = std::filesystem;

namespace {

const char* kReset = "\033[0m";
const char* kCyan = "\033[36m";
const char* kBold = "\033[1m";
const char* kDim = "\033[2m";
const char* kGreen = "\033[32m";
const char* kRed = "\033[31m";

std::string env_or(const std::string& name, const std::string& fallback = "") {
    const char* v = std::getenv(name.c_str());
    return (v && *v) ? std::string(v) : fallback;
}

std::string pad(std::string s, std::size_t w) {
    if (s.size() >= w) return s;
    s.append(w - s.size(), ' ');
    return s;
}

}  // namespace

std::string check_mark(bool ok, bool color) {
    if (!color) return ok ? "ok" : "--";
    std::string out;
    out += ok ? kGreen : kRed;
    out += ok ? "\u2713" : "\u2717";
    out += kReset;
    return out;
}

std::string redact_key(const std::string& key) {
    if (key.empty()) return "(not set)";
    if (key.size() < 12) return "***";
    return key.substr(0, 4) + "..." + key.substr(key.size() - 4);
}

std::string format_iso_timestamp(const std::string& value) {
    if (value.empty()) return "(unknown)";
    std::string text = value;
    if (!text.empty() && text.back() == 'Z') {
        text.pop_back();
    }
    std::tm tm_buf{};
    std::istringstream is(text);
    is >> std::get_time(&tm_buf, "%Y-%m-%dT%H:%M:%S");
    if (is.fail()) return value;
#if defined(_WIN32)
    std::time_t utc = _mkgmtime(&tm_buf);
#else
    std::time_t utc = timegm(&tm_buf);
#endif
    std::tm local_buf{};
#if defined(_WIN32)
    localtime_s(&local_buf, &utc);
#else
    localtime_r(&utc, &local_buf);
#endif
    char buf[64];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S %Z", &local_buf);
    return buf;
}

std::string configured_model_label(const nlohmann::json& config) {
    if (!config.is_object() || !config.contains("model")) return "(not set)";
    const auto& m = config["model"];
    std::string value;
    if (m.is_string()) value = m.get<std::string>();
    else if (m.is_object()) {
        if (m.contains("default") && m["default"].is_string())
            value = m["default"].get<std::string>();
        else if (m.contains("name") && m["name"].is_string())
            value = m["name"].get<std::string>();
    }
    if (value.empty()) return "(not set)";
    return value;
}

std::string effective_provider_label(const nlohmann::json& config) {
    std::string provider;
    if (config.is_object() && config.contains("provider") &&
        config["provider"].is_string()) {
        provider = config["provider"].get<std::string>();
    }
    if (provider.empty()) {
        const char* env = std::getenv("HERMES_PROVIDER");
        if (env && *env) provider = env;
    }
    if (provider == "openrouter" && !env_or("OPENAI_BASE_URL").empty()) {
        return "custom";
    }
    return provider.empty() ? "auto" : provider;
}

std::string effective_terminal_backend(const nlohmann::json& config) {
    std::string v = env_or("TERMINAL_ENV");
    if (!v.empty()) return v;
    if (config.is_object() && config.contains("terminal") &&
        config["terminal"].is_object() &&
        config["terminal"].contains("backend") &&
        config["terminal"]["backend"].is_string()) {
        return config["terminal"]["backend"].get<std::string>();
    }
    return "local";
}

const std::vector<ApiKeyRow>& api_key_rows() {
    static const std::vector<ApiKeyRow> kRows = {
        {"OpenRouter",   "OPENROUTER_API_KEY"},
        {"OpenAI",       "OPENAI_API_KEY"},
        {"Z.AI/GLM",     "GLM_API_KEY"},
        {"Kimi",         "KIMI_API_KEY"},
        {"MiniMax",      "MINIMAX_API_KEY"},
        {"MiniMax-CN",   "MINIMAX_CN_API_KEY"},
        {"Firecrawl",    "FIRECRAWL_API_KEY"},
        {"Tavily",       "TAVILY_API_KEY"},
        {"Browser Use",  "BROWSER_USE_API_KEY"},
        {"Browserbase",  "BROWSERBASE_API_KEY"},
        {"FAL",          "FAL_KEY"},
        {"Tinker",       "TINKER_API_KEY"},
        {"WandB",        "WANDB_API_KEY"},
        {"ElevenLabs",   "ELEVENLABS_API_KEY"},
        {"GitHub",       "GITHUB_TOKEN"},
        {"Anthropic",    "ANTHROPIC_API_KEY"},
    };
    return kRows;
}

const std::vector<ApiKeyProvider>& api_key_providers() {
    static const std::vector<ApiKeyProvider> kProviders = {
        {"Z.AI / GLM",      {"GLM_API_KEY", "ZAI_API_KEY", "Z_AI_API_KEY"}},
        {"Kimi / Moonshot", {"KIMI_API_KEY"}},
        {"MiniMax",         {"MINIMAX_API_KEY"}},
        {"MiniMax (China)", {"MINIMAX_CN_API_KEY"}},
    };
    return kProviders;
}

const std::vector<PlatformRow>& messaging_platforms() {
    static const std::vector<PlatformRow> kRows = {
        {"Telegram",   "TELEGRAM_BOT_TOKEN",     "TELEGRAM_HOME_CHANNEL"},
        {"Discord",    "DISCORD_BOT_TOKEN",      "DISCORD_HOME_CHANNEL"},
        {"WhatsApp",   "WHATSAPP_ENABLED",       ""},
        {"Signal",     "SIGNAL_HTTP_URL",        "SIGNAL_HOME_CHANNEL"},
        {"Slack",      "SLACK_BOT_TOKEN",        ""},
        {"Email",      "EMAIL_ADDRESS",          "EMAIL_HOME_ADDRESS"},
        {"SMS",        "TWILIO_ACCOUNT_SID",     "SMS_HOME_CHANNEL"},
        {"DingTalk",   "DINGTALK_CLIENT_ID",     ""},
        {"Feishu",     "FEISHU_APP_ID",          "FEISHU_HOME_CHANNEL"},
        {"WeCom",      "WECOM_BOT_ID",           "WECOM_HOME_CHANNEL"},
        {"Weixin",     "WEIXIN_ACCOUNT_ID",      "WEIXIN_HOME_CHANNEL"},
        {"BlueBubbles","BLUEBUBBLES_SERVER_URL", "BLUEBUBBLES_HOME_CHANNEL"},
    };
    return kRows;
}

// --- Counts -----------------------------------------------------------------

std::size_t count_active_sessions() {
    auto file = hermes::core::path::get_hermes_home() / "sessions" / "sessions.json";
    std::error_code ec;
    if (!fs::exists(file, ec)) return 0;
    std::ifstream f(file);
    if (!f) return 0;
    try {
        auto j = nlohmann::json::parse(f);
        if (j.is_array()) return j.size();
        if (j.is_object()) return j.size();
    } catch (...) {}
    return 0;
}

std::size_t count_cron_jobs(bool enabled_only) {
    auto file = hermes::core::path::get_hermes_home() / "cron" / "jobs.json";
    std::error_code ec;
    if (!fs::exists(file, ec)) return 0;
    std::ifstream f(file);
    if (!f) return 0;
    try {
        auto j = nlohmann::json::parse(f);
        if (!j.contains("jobs") || !j["jobs"].is_array()) return 0;
        if (!enabled_only) return j["jobs"].size();
        std::size_t n = 0;
        for (const auto& job : j["jobs"]) {
            if (!job.contains("enabled") || job["enabled"].get<bool>()) ++n;
        }
        return n;
    } catch (...) {}
    return 0;
}

// --- Rendering --------------------------------------------------------------

void render_environment(std::ostream& out, const nlohmann::json& config,
                        const Options& opts) {
    (void)opts;
    out << "\n" << kCyan << kBold << "\u25c6 Environment" << kReset << "\n";
    out << "  Project:      " << fs::current_path().string() << "\n";
    out << "  Runtime:      C++17\n";

    auto env_path = hermes::core::path::get_hermes_home() / ".env";
    bool env_exists = fs::exists(env_path);
    out << "  .env file:    " << check_mark(env_exists, opts.color) << " "
        << (env_exists ? "exists" : "not found") << "\n";

    out << "  Model:        " << configured_model_label(config) << "\n";
    out << "  Provider:     " << effective_provider_label(config) << "\n";
}

void render_api_keys(std::ostream& out, const Options& opts) {
    out << "\n" << kCyan << kBold << "\u25c6 API Keys" << kReset << "\n";
    for (const auto& row : api_key_rows()) {
        std::string v = env_or(row.env_var);
        std::string display = opts.show_all ? v : redact_key(v);
        out << "  " << pad(row.label, 12) << "  "
            << check_mark(!v.empty(), opts.color) << " " << display << "\n";
    }
}

void render_apikey_providers(std::ostream& out, const Options& opts) {
    out << "\n" << kCyan << kBold << "\u25c6 API-Key Providers" << kReset << "\n";
    for (const auto& prov : api_key_providers()) {
        std::string value;
        for (const auto& env : prov.env_vars) {
            value = env_or(env);
            if (!value.empty()) break;
        }
        bool ok = !value.empty();
        out << "  " << pad(prov.label, 16) << " "
            << check_mark(ok, opts.color) << " "
            << (ok ? "configured" : "not configured") << "\n";
    }
}

void render_messaging_platforms(std::ostream& out, const Options& opts) {
    out << "\n" << kCyan << kBold << "\u25c6 Messaging Platforms" << kReset << "\n";
    for (const auto& p : messaging_platforms()) {
        std::string token = env_or(p.token_env);
        bool has_token = !token.empty();
        std::string home = p.home_env.empty() ? "" : env_or(p.home_env);
        std::string status = has_token ? "configured" : "not configured";
        if (!home.empty()) status += " (home: " + home + ")";
        out << "  " << pad(p.label, 12) << "  "
            << check_mark(has_token, opts.color) << " " << status << "\n";
    }
}

void render_terminal_backend(std::ostream& out, const nlohmann::json& config,
                             const Options& opts) {
    out << "\n" << kCyan << kBold << "\u25c6 Terminal Backend" << kReset << "\n";
    std::string backend = effective_terminal_backend(config);
    out << "  Backend:      " << backend << "\n";
    if (backend == "ssh") {
        out << "  SSH Host:     " << env_or("TERMINAL_SSH_HOST", "(not set)") << "\n";
        out << "  SSH User:     " << env_or("TERMINAL_SSH_USER", "(not set)") << "\n";
    } else if (backend == "docker") {
        out << "  Docker Image: "
            << env_or("TERMINAL_DOCKER_IMAGE", "python:3.11-slim") << "\n";
    } else if (backend == "daytona") {
        out << "  Daytona Image: "
            << env_or("TERMINAL_DAYTONA_IMAGE", "nikolaik/python-nodejs:python3.11-nodejs20")
            << "\n";
    }
    std::string sudo = env_or("SUDO_PASSWORD");
    out << "  Sudo:         " << check_mark(!sudo.empty(), opts.color) << " "
        << (sudo.empty() ? "disabled" : "enabled") << "\n";
}

void render_session_summary(std::ostream& out, const Options& opts) {
    (void)opts;
    out << "\n" << kCyan << kBold << "\u25c6 Sessions" << kReset << "\n";
    auto n = count_active_sessions();
    out << "  Active:       " << n << " session(s)\n";
}

void render_cron_summary(std::ostream& out, const Options& opts) {
    (void)opts;
    out << "\n" << kCyan << kBold << "\u25c6 Scheduled Jobs" << kReset << "\n";
    auto total = count_cron_jobs(false);
    auto enabled = count_cron_jobs(true);
    out << "  Jobs:         " << enabled << " active, " << total << " total\n";
}

int cmd_status(std::ostream& out, const Options& opts) {
    nlohmann::json config;
    try {
        config = hermes::config::load_config();
    } catch (...) {
        config = nlohmann::json::object();
    }

    out << "\n";
    out << kCyan << "\u250c" << std::string(58, '-') << "\u2510" << kReset << "\n";
    out << kCyan << "\u2502" << "   \u2695 Hermes Agent Status" << kReset << "\n";
    out << kCyan << "\u2514" << std::string(58, '-') << "\u2518" << kReset << "\n";

    render_environment(out, config, opts);
    render_api_keys(out, opts);
    render_apikey_providers(out, opts);
    render_terminal_backend(out, config, opts);
    render_messaging_platforms(out, opts);
    render_cron_summary(out, opts);
    render_session_summary(out, opts);

    out << "\n" << kDim << std::string(60, '-') << kReset << "\n";
    out << kDim << "  Run 'hermes doctor' for detailed diagnostics" << kReset << "\n";
    out << kDim << "  Run 'hermes setup' to configure" << kReset << "\n\n";
    return 0;
}

int dispatch(int argc, char** argv) {
    Options opts;
    for (int i = 2; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--all") opts.show_all = true;
        else if (a == "--deep") opts.deep = true;
        else if (a == "--no-color") opts.color = false;
    }
    return cmd_status(std::cout, opts);
}

}  // namespace hermes::cli::status
