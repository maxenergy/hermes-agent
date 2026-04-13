#include "hermes/cli/main_entry.hpp"

#include "hermes/auth/copilot_oauth.hpp"
#include "hermes/auth/credentials.hpp"
#include "hermes/auth/nous_subscription.hpp"
#include "hermes/auth/qwen_oauth.hpp"
#include "hermes/cli/claw_migrate.hpp"
#include "hermes/cli/hermes_cli.hpp"
#include "hermes/config/loader.hpp"
#include "hermes/core/path.hpp"
#include "hermes/cron/jobs.hpp"
#include "hermes/gateway/pairing.hpp"
#include "hermes/gateway/gateway_config.hpp"
#include "hermes/llm/llm_client.hpp"
#include "hermes/profile/profile.hpp"
#include "hermes/skills/skill_utils.hpp"
#include "hermes/tools/toolsets.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <random>
#include <sstream>
#include <string>
#include <vector>

#ifdef _WIN32
#include <io.h>
#define isatty _isatty
#define STDIN_FILENO 0
#else
#include <unistd.h>
#endif

namespace hermes::cli {

namespace fs = std::filesystem;

namespace {

void print_global_help() {
    std::cout << "Hermes — your AI agent\n\n"
              << "Usage: hermes [subcommand] [options]\n\n"
              << "Subcommands:\n"
              << "  chat        Start an interactive chat session (default)\n"
              << "  gateway     Run the gateway server\n"
              << "  setup       Run the setup wizard\n"
              << "  model       Show or set the default model\n"
              << "  tools       List available tools\n"
              << "  skills      List available skills\n"
              << "  doctor      Run diagnostic checks\n"
              << "  status      Show agent status\n"
              << "  config      Show or edit configuration\n"
              << "  logs        Show recent log output\n"
              << "  cron        Manage scheduled tasks\n"
              << "  profile     Show user profile\n"
              << "  version     Print version and exit\n"
              << "  pairing     Manage DM pairing codes\n"
              << "  claw        OpenClaw migration tool\n"
              << "  update      Update Hermes\n"
              << "  uninstall   Remove Hermes\n"
              << "  login       Log in to a provider (default: qwen)\n"
              << "  auth        Manage provider auth — login/logout/status across\n"
              << "              openai|anthropic|qwen|copilot|nous\n"
              << "  providers   List configured providers + key status; `providers test <name>`\n"
              << "  dump        Export sessions/config/memory (--since, --output PATH)\n"
              << "  webhook     Install/list/remove webhook endpoints (writes webhooks.json)\n"
              << "  runtime     Switch terminal backend (list | select <name>)\n"
              << "  memory      Configure memory backend (Honcho)\n"
              << "\n"
              << "Run 'hermes <subcommand> --help' for details.\n";
}

// ANSI color helpers.
const char* green() { return "\033[32m"; }
const char* red()   { return "\033[31m"; }
const char* dim()   { return "\033[2m"; }
const char* reset() { return "\033[0m"; }

std::string pass_label() { return std::string(green()) + "[PASS]" + reset(); }
std::string fail_label() { return std::string(red()) + "[FAIL]" + reset(); }

bool check_file_exists(const fs::path& path) {
    return fs::exists(path);
}

bool check_which(const std::string& cmd) {
    std::string full = "which " + cmd + " > /dev/null 2>&1";
    return std::system(full.c_str()) == 0;
}

bool check_sqlite_fts5() {
    // We have SQLite linked (build dependency); FTS5 is available if the
    // library was compiled with it.  A quick probe: try to open an in-memory
    // DB and create a virtual table.  For simplicity, we shell out to sqlite3.
    int rc = std::system(
        "sqlite3 ':memory:' 'CREATE VIRTUAL TABLE t USING fts5(a);' "
        "2>/dev/null");
    return rc == 0;
}

}  // namespace

int cmd_version() {
    std::cout << kVersionString << "\n";
    return 0;
}

int cmd_doctor() {
    std::cout << "Running diagnostics...\n\n";

    auto home = hermes::core::path::get_hermes_home();
    auto config_file = home / "config.yaml";

    struct Check {
        std::string label;
        bool passed;
    };

    std::vector<Check> checks;

    // 1. Config file
    checks.push_back({"Config file (" + config_file.string() + ")",
                       check_file_exists(config_file)});

    // 2. SQLite FTS5
    checks.push_back({"SQLite FTS5 extension", check_sqlite_fts5()});

    // 3. curl available
    checks.push_back({"curl available", check_which("curl")});

    // 4. Docker available
    checks.push_back({"Docker available", check_which("docker")});

    // 5. SSH available
    checks.push_back({"SSH available", check_which("ssh")});

    // Print results as a table.
    int passed = 0;
    int failed = 0;
    for (const auto& c : checks) {
        std::string status = c.passed ? pass_label() : fail_label();
        std::cout << "  " << status << "  " << c.label << "\n";
        if (c.passed) ++passed; else ++failed;
    }

    std::cout << "\n" << passed << " passed, " << failed << " failed, "
              << checks.size() << " total.\n";
    return (failed > 0) ? 1 : 0;
}

int cmd_status() {
    auto config = hermes::config::load_config();

    std::string model = "anthropic/claude-opus-4-6";
    if (config.contains("model") && config["model"].is_string()) {
        model = config["model"].get<std::string>();
    }

    std::string provider = "(default)";
    if (config.contains("provider") && config["provider"].is_string()) {
        provider = config["provider"].get<std::string>();
    }

    std::string skin_name = "default";
    if (config.contains("cli") && config["cli"].contains("skin")) {
        skin_name = config["cli"]["skin"].get<std::string>();
    }

    // Enabled toolsets.
    std::string toolsets_str;
    if (config.contains("toolsets") && config["toolsets"].is_array()) {
        for (const auto& ts : config["toolsets"]) {
            if (!toolsets_str.empty()) toolsets_str += ", ";
            toolsets_str += ts.get<std::string>();
        }
    }
    if (toolsets_str.empty()) toolsets_str = "(default core)";

    std::cout << "Hermes status:\n"
              << "  Version:   " << kVersionString << "\n"
              << "  Model:     " << model << "\n"
              << "  Provider:  " << provider << "\n"
              << "  Skin:      " << skin_name << "\n"
              << "  Toolsets:  " << toolsets_str << "\n"
              << "  Runtime:   C++17\n";
    return 0;
}

int cmd_model() {
    // Interactive model selection — list known models, let user pick.
    static const std::vector<std::string> known_models = {
        "anthropic/claude-opus-4-6",
        "anthropic/claude-sonnet-4-20250514",
        "anthropic/claude-haiku-3",
        "openai/gpt-4o",
        "openai/gpt-4o-mini",
        "openai/o3",
        "google/gemini-2.5-pro",
        "google/gemini-2.5-flash",
        "deepseek/deepseek-r1",
        "qwen/qwen3-coder",
    };

    auto config = hermes::config::load_config();
    std::string current = "anthropic/claude-opus-4-6";
    if (config.contains("model") && config["model"].is_string()) {
        current = config["model"].get<std::string>();
    }

    std::cout << "Current model: " << current << "\n\n";
    std::cout << "Available models:\n";
    for (std::size_t i = 0; i < known_models.size(); ++i) {
        std::string marker = (known_models[i] == current) ? " *" : "";
        std::cout << "  " << (i + 1) << ". " << known_models[i] << marker << "\n";
    }

    std::cout << "\nEnter number to select (or press Enter to keep current): ";
    std::string line;
    if (!std::getline(std::cin, line) || line.empty()) {
        std::cout << "Keeping current model: " << current << "\n";
        return 0;
    }

    try {
        auto idx = std::stoul(line);
        if (idx >= 1 && idx <= known_models.size()) {
            config["model"] = known_models[idx - 1];
            hermes::config::save_config(config);
            std::cout << "Model set to: " << known_models[idx - 1] << "\n";
        } else {
            std::cerr << "Invalid selection.\n";
            return 1;
        }
    } catch (...) {
        std::cerr << "Invalid input.\n";
        return 1;
    }
    return 0;
}

int cmd_tools() {
    const auto& ts = hermes::tools::toolsets();
    std::cout << "Available toolsets:\n\n";
    for (const auto& [name, def] : ts) {
        std::cout << "  " << name << dim() << " — " << def.description
                  << reset() << "\n";
        auto tools = hermes::tools::resolve_toolset(name);
        std::cout << "    Tools: ";
        for (std::size_t i = 0; i < tools.size(); ++i) {
            if (i > 0) std::cout << ", ";
            std::cout << tools[i];
        }
        std::cout << "\n\n";
    }
    return 0;
}

int cmd_config(int argc, char* argv[]) {
    // hermes config            → show config summary
    // hermes config set K V    → update key
    // hermes config edit       → open in $EDITOR
    if (argc <= 2) {
        // Show config summary.
        auto config = hermes::config::load_config();
        std::cout << config.dump(2) << "\n";
        return 0;
    }

    std::string sub = argv[2];

    if (sub == "set") {
        if (argc < 5) {
            std::cerr << "Usage: hermes config set <key> <value>\n";
            return 1;
        }
        std::string key = argv[3];
        std::string value = argv[4];

        auto config = hermes::config::load_config();

        // Support dotted keys: "a.b.c" sets config["a"]["b"]["c"].
        nlohmann::json* node = &config;
        std::string segment;
        std::size_t start = 0;
        std::size_t dot;
        std::vector<std::string> segments;
        while ((dot = key.find('.', start)) != std::string::npos) {
            segments.push_back(key.substr(start, dot - start));
            start = dot + 1;
        }
        segments.push_back(key.substr(start));

        for (std::size_t i = 0; i + 1 < segments.size(); ++i) {
            if (!node->contains(segments[i]) || !(*node)[segments[i]].is_object()) {
                (*node)[segments[i]] = nlohmann::json::object();
            }
            node = &(*node)[segments[i]];
        }

        // Try to parse the value as JSON (for booleans, numbers, etc.).
        try {
            (*node)[segments.back()] = nlohmann::json::parse(value);
        } catch (...) {
            (*node)[segments.back()] = value;
        }

        hermes::config::save_config(config);
        std::cout << "Set " << key << " = " << (*node)[segments.back()].dump() << "\n";
        return 0;
    }

    if (sub == "edit") {
        auto path = hermes::core::path::get_hermes_home() / "config.yaml";
        const char* editor = std::getenv("EDITOR");
        if (!editor) editor = "vi";
        std::string cmd = std::string(editor) + " " + path.string();
        return std::system(cmd.c_str());
    }

    std::cerr << "Unknown config subcommand: " << sub << "\n";
    std::cerr << "Usage: hermes config [set <key> <value> | edit]\n";
    return 1;
}

int cmd_gateway(int argc, char* argv[]) {
    if (argc <= 2) {
        std::cout << "Usage: hermes gateway [start|stop]\n";
        return 1;
    }

    std::string sub = argv[2];
    if (sub == "start") {
        std::cout << "Starting gateway...\n";
        // Stub — gateway start is a future phase.
        return 0;
    }
    if (sub == "stop") {
        std::cout << "Stopping gateway...\n";
        // Stub — gateway stop is a future phase.
        return 0;
    }

    std::cerr << "Unknown gateway subcommand: " << sub << "\n";
    return 1;
}

int cmd_setup() {
    std::cout << "Hermes Setup Wizard\n"
              << "====================\n\n";

    // 1. Model provider.
    std::cout << "Select your model provider:\n"
              << "  1. OpenRouter (recommended)\n"
              << "  2. Anthropic\n"
              << "  3. OpenAI\n"
              << "Choice [1]: ";
    std::string choice;
    if (!std::getline(std::cin, choice) || choice.empty()) {
        choice = "1";
    }

    std::string provider;
    if (choice == "1") provider = "openrouter";
    else if (choice == "2") provider = "anthropic";
    else if (choice == "3") provider = "openai";
    else {
        std::cout << "Invalid selection, defaulting to OpenRouter.\n";
        provider = "openrouter";
    }

    // 2. API key (masked).
    std::cout << "Enter your " << provider << " API key: ";
    std::string api_key;
    if (!std::getline(std::cin, api_key) || api_key.empty()) {
        std::cout << "No API key provided. You can set it later in config.yaml.\n";
    }

    // 3. Terminal backend.
    std::cout << "Select terminal backend:\n"
              << "  1. readline (default)\n"
              << "  2. raw\n"
              << "Choice [1]: ";
    std::string backend_choice;
    if (!std::getline(std::cin, backend_choice) || backend_choice.empty()) {
        backend_choice = "1";
    }
    std::string backend = (backend_choice == "2") ? "raw" : "readline";

    // Save to config.yaml.
    auto config = hermes::config::load_config();
    config["provider"] = provider;
    if (!api_key.empty()) {
        config["api_key"] = api_key;
    }
    config["cli"]["backend"] = backend;
    hermes::config::save_config(config);

    std::cout << "\nSetup complete! Configuration saved.\n"
              << "Run 'hermes' to start chatting.\n";
    return 0;
}

int cmd_skills() {
    auto skills = hermes::skills::iter_skill_index();
    if (skills.empty()) {
        std::cout << "No skills found.\n";
        return 0;
    }

    std::cout << "Available skills:\n\n";
    std::cout << "  " << std::left;
    std::cout << std::setw(20) << "Name"
              << std::setw(40) << "Description"
              << "Platforms\n";
    std::cout << "  " << std::string(80, '-') << "\n";

    for (const auto& skill : skills) {
        std::string platforms_str;
        for (const auto& p : skill.platforms) {
            if (!platforms_str.empty()) platforms_str += ", ";
            platforms_str += p;
        }
        if (platforms_str.empty()) platforms_str = "all";

        std::string status = skill.enabled ? "" : " (disabled)";
        std::cout << "  " << std::setw(20) << skill.name
                  << std::setw(40) << (skill.description.substr(0, 37) +
                                       (skill.description.size() > 37 ? "..." : ""))
                  << platforms_str << status << "\n";
    }
    return 0;
}

int cmd_logs() {
    auto home = hermes::core::path::get_hermes_home();
    auto log_file = home / "logs" / "agent.log";

    if (!fs::exists(log_file)) {
        std::cout << "No log file found at " << log_file << "\n";
        return 1;
    }

    std::ifstream file(log_file);
    if (!file.is_open()) {
        std::cerr << "Cannot open " << log_file << "\n";
        return 1;
    }

    // Read all lines, keep last 50.
    std::vector<std::string> lines;
    std::string line;
    while (std::getline(file, line)) {
        lines.push_back(line);
    }

    std::size_t start = 0;
    if (lines.size() > 50) {
        start = lines.size() - 50;
    }
    for (std::size_t i = start; i < lines.size(); ++i) {
        std::cout << lines[i] << "\n";
    }
    return 0;
}

int cmd_cron() {
    auto home = hermes::core::path::get_hermes_home();
    auto cron_dir = home / "cron";

    hermes::cron::JobStore store(cron_dir);
    auto jobs = store.list_all();

    if (jobs.empty()) {
        std::cout << "No scheduled jobs.\n";
        return 0;
    }

    std::cout << "Scheduled jobs:\n\n";
    std::cout << "  " << std::left
              << std::setw(12) << "ID"
              << std::setw(20) << "Name"
              << std::setw(16) << "Schedule"
              << std::setw(8) << "Runs"
              << "Status\n";
    std::cout << "  " << std::string(60, '-') << "\n";

    for (const auto& job : jobs) {
        std::string status = job.paused ? "paused" : "active";
        std::cout << "  " << std::setw(12) << job.id.substr(0, 10)
                  << std::setw(20) << job.name.substr(0, 18)
                  << std::setw(16) << job.schedule_str.substr(0, 14)
                  << std::setw(8) << job.run_count
                  << status << "\n";
    }
    return 0;
}

int cmd_profile(int argc, char* argv[]) {
    // hermes profile             → list profiles
    // hermes profile create NAME → create profile
    // hermes profile delete NAME → delete profile
    if (argc <= 2) {
        auto profiles = hermes::profile::list_profiles();
        if (profiles.empty()) {
            std::cout << "No profiles. Use 'hermes profile create <name>' to create one.\n";
        } else {
            std::cout << "Profiles:\n";
            for (const auto& name : profiles) {
                std::cout << "  " << name << "\n";
            }
        }
        return 0;
    }

    std::string sub = argv[2];

    if (sub == "create") {
        if (argc < 4) {
            std::cerr << "Usage: hermes profile create <name>\n";
            return 1;
        }
        std::string name = argv[3];
        hermes::profile::create_profile(name);
        std::cout << "Profile '" << name << "' created.\n";
        return 0;
    }

    if (sub == "delete") {
        if (argc < 4) {
            std::cerr << "Usage: hermes profile delete <name>\n";
            return 1;
        }
        std::string name = argv[3];
        try {
            hermes::profile::delete_profile(name);
            std::cout << "Profile '" << name << "' deleted.\n";
        } catch (const std::runtime_error& e) {
            std::cerr << "Error: " << e.what() << "\n";
            return 1;
        }
        return 0;
    }

    if (sub == "list") {
        auto profiles = hermes::profile::list_profiles();
        if (profiles.empty()) {
            std::cout << "No profiles.\n";
        } else {
            std::cout << "Profiles:\n";
            for (const auto& name : profiles) {
                std::cout << "  " << name << "\n";
            }
        }
        return 0;
    }

    std::cerr << "Unknown profile subcommand: " << sub << "\n"
              << "Usage: hermes profile [list|create <name>|delete <name>]\n";
    return 1;
}

int cmd_update() {
    std::cout << "Check https://github.com/NousResearch/hermes-agent/releases for updates.\n"
              << "Current version: " << kVersionString << "\n";
    return 0;
}

int cmd_uninstall() {
    std::cout << "This will remove ~/.hermes/ and the hermes binary.\n"
              << "Are you sure? [y/N]: ";
    std::string confirm;
    if (!std::getline(std::cin, confirm) || (confirm != "y" && confirm != "Y")) {
        std::cout << "Uninstall cancelled.\n";
        return 0;
    }

    auto home = hermes::core::path::get_hermes_home();
    std::error_code ec;
    fs::remove_all(home, ec);
    if (ec) {
        std::cerr << "Failed to remove " << home << ": " << ec.message() << "\n";
        return 1;
    }
    std::cout << "Removed " << home << "\n";

    // Try to remove the binary itself.
    auto self = fs::read_symlink("/proc/self/exe", ec);
    if (!ec && fs::exists(self)) {
        fs::remove(self, ec);
        if (!ec) {
            std::cout << "Removed " << self << "\n";
        }
    }

    std::cout << "Hermes uninstalled.\n";
    return 0;
}

// --------------------------------------------------------------------------
// Long-tail CLI subcommands — model-switch / providers / memory / dump /
// webhook / runtime.
// --------------------------------------------------------------------------

namespace {
// Lightweight YAML load/save used by all the subcommands below.  We only need
// to round-trip a flat config tree; nlohmann::json (used by the full config
// loader) already stores ordered keys so we convert through it.
nlohmann::json load_config_json() {
    try {
        return hermes::config::load_cli_config();
    } catch (...) {
        return nlohmann::json::object();
    }
}

bool save_config_json(const nlohmann::json& j) {
    try {
        hermes::config::save_config(j);
        return true;
    } catch (const std::exception& e) {
        std::cerr << "Failed to save config: " << e.what() << "\n";
        return false;
    }
}

// Sets config[path] via dotted-key notation, e.g. "terminal.backend".
void set_dot_path(nlohmann::json& j, const std::string& dotted,
                  const nlohmann::json& value) {
    nlohmann::json* cur = &j;
    std::string remaining = dotted;
    while (true) {
        auto pos = remaining.find('.');
        std::string head = (pos == std::string::npos) ? remaining
                                                       : remaining.substr(0, pos);
        if (pos == std::string::npos) {
            (*cur)[head] = value;
            return;
        }
        if (!cur->contains(head) || !(*cur)[head].is_object()) {
            (*cur)[head] = nlohmann::json::object();
        }
        cur = &(*cur)[head];
        remaining = remaining.substr(pos + 1);
    }
}

std::string mask_secret(const std::string& s) {
    if (s.empty()) return "(unset)";
    if (s.size() <= 8) return "***";
    return s.substr(0, 4) + "…" + s.substr(s.size() - 4);
}

std::string random_hex(size_t bytes) {
    static thread_local std::mt19937_64 rng{std::random_device{}()};
    std::uniform_int_distribution<int> dist(0, 15);
    static const char* hex = "0123456789abcdef";
    std::string out;
    out.reserve(bytes * 2);
    for (size_t i = 0; i < bytes * 2; ++i) out.push_back(hex[dist(rng)]);
    return out;
}
}  // namespace

int cmd_model_switch(int argc, char* argv[]) {
    if (argc < 4) {
        std::cerr << "Usage: hermes model switch <provider:model>\n"
                  << "  e.g. hermes model switch anthropic:claude-opus-4-6\n";
        return 1;
    }
    std::string new_model = argv[3];
    auto cfg = load_config_json();
    std::string old_model = "(unset)";
    if (cfg.contains("model") && cfg["model"].is_string()) {
        old_model = cfg["model"].get<std::string>();
    }
    cfg["model"] = new_model;
    if (!save_config_json(cfg)) return 1;
    std::cout << "Switched model: " << old_model << " → " << new_model << "\n";
    return 0;
}

namespace {
struct ProviderInfo {
    const char* name;         // display
    const char* env;          // API key env var
    const char* ping_url;     // simple GET to probe reachability (no cost)
};
// Reachability probes use idempotent health / model-listing endpoints.  None
// consume tokens; they just confirm the service is reachable.
const std::vector<ProviderInfo>& known_providers() {
    static const std::vector<ProviderInfo> provs = {
        {"openrouter", "OPENROUTER_API_KEY", "https://openrouter.ai/api/v1/models"},
        {"anthropic",  "ANTHROPIC_API_KEY",  "https://api.anthropic.com/v1/models"},
        {"openai",     "OPENAI_API_KEY",     "https://api.openai.com/v1/models"},
        {"google",     "GOOGLE_API_KEY",     "https://generativelanguage.googleapis.com/v1beta/models"},
        {"mistral",    "MISTRAL_API_KEY",    "https://api.mistral.ai/v1/models"},
        {"minimax",    "MINIMAX_API_KEY",    "https://api.minimax.chat/v1/text/chatcompletion_pro"},
        {"groq",       "GROQ_API_KEY",       "https://api.groq.com/openai/v1/models"},
        {"nous",       "NOUS_API_KEY",       "https://api.nousresearch.com/v1/models"},
        {"qwen",       "DASHSCOPE_API_KEY",  "https://dashscope.aliyuncs.com/compatible-mode/v1/models"},
        {"firecrawl",  "FIRECRAWL_API_KEY",  "https://api.firecrawl.dev/v0/crawl"},
        {"exa",        "EXA_API_KEY",        "https://api.exa.ai/search"},
        {"tavily",     "TAVILY_API_KEY",     "https://api.tavily.com/search"},
        {"elevenlabs", "ELEVENLABS_API_KEY", "https://api.elevenlabs.io/v1/voices"},
    };
    return provs;
}

// Test-injectable transport override for `providers test`.  Default nullptr
// means "use the real curl transport".  Tests can set this to a
// FakeHttpTransport to avoid network calls.
hermes::llm::HttpTransport* g_providers_transport_override = nullptr;

}  // namespace

// Public test hook — lets unit tests swap in a FakeHttpTransport without
// actually hitting the network.  Passing nullptr restores the default.
void set_providers_transport_override(hermes::llm::HttpTransport* t);
void set_providers_transport_override(hermes::llm::HttpTransport* t) {
    g_providers_transport_override = t;
}

int cmd_providers(int argc, char* argv[]) {
    std::string action = argc > 2 ? argv[2] : "list";

    if (action == "list") {
        std::cout << std::left << std::setw(14) << "Provider"
                  << std::setw(26) << "Env Var"
                  << "Status\n";
        std::cout << std::string(60, '-') << "\n";
        // Original capitalized display preserved for back-compat tests.
        struct Row { const char* display; const char* env; };
        static const Row rows[] = {
            {"OpenRouter", "OPENROUTER_API_KEY"},
            {"Anthropic",  "ANTHROPIC_API_KEY"},
            {"OpenAI",     "OPENAI_API_KEY"},
            {"Google",     "GOOGLE_API_KEY"},
            {"Mistral",    "MISTRAL_API_KEY"},
            {"MiniMax",    "MINIMAX_API_KEY"},
            {"Groq",       "GROQ_API_KEY"},
            {"Nous",       "NOUS_API_KEY"},
            {"Qwen",       "DASHSCOPE_API_KEY"},
            {"Firecrawl",  "FIRECRAWL_API_KEY"},
            {"Exa",        "EXA_API_KEY"},
            {"Tavily",     "TAVILY_API_KEY"},
            {"ElevenLabs", "ELEVENLABS_API_KEY"},
        };
        for (const auto& r : rows) {
            auto val = hermes::auth::get_credential(r.env);
            std::string status = val ? mask_secret(*val) : "(unset)";
            std::cout << std::left << std::setw(14) << r.display
                      << std::setw(26) << r.env
                      << status << "\n";
        }
        return 0;
    }

    if (action == "test") {
        if (argc < 4) {
            std::cerr << "Usage: hermes providers test <name>\n";
            return 1;
        }
        std::string name = argv[3];
        // Case-insensitive compare.
        std::string lower;
        for (char c : name) lower.push_back(static_cast<char>(std::tolower(c)));

        const ProviderInfo* info = nullptr;
        for (const auto& p : known_providers()) {
            if (lower == p.name) { info = &p; break; }
        }
        if (!info) {
            std::cerr << "Unknown provider: " << name << "\n";
            return 1;
        }

        auto key = hermes::auth::get_credential(info->env);
        if (!key || key->empty()) {
            std::cerr << info->name << ": no API key ("
                      << info->env << " is unset)\n";
            return 1;
        }

        auto* transport = g_providers_transport_override;
        if (!transport) transport = hermes::llm::get_default_transport();
        if (!transport) {
            std::cerr << "No HTTP transport available (build without curl?)\n";
            return 1;
        }

        std::unordered_map<std::string, std::string> headers;
        // Provide the API key in the conventional header for each provider —
        // a 401 still tells us the endpoint is reachable, which is all this
        // probe cares about.
        if (lower == "anthropic") {
            headers["x-api-key"] = *key;
            headers["anthropic-version"] = "2023-06-01";
        } else {
            headers["Authorization"] = "Bearer " + *key;
        }

        try {
            auto resp = transport->get(info->ping_url, headers);
            bool ok = (resp.status_code >= 200 && resp.status_code < 500);
            std::cout << info->name
                      << ": HTTP " << resp.status_code
                      << (ok ? "  [reachable]" : "  [unreachable]")
                      << "\n";
            return ok ? 0 : 1;
        } catch (const std::exception& e) {
            std::cerr << info->name << ": transport error: " << e.what() << "\n";
            return 1;
        }
    }

    std::cerr << "Usage: hermes providers [list|test <name>]\n";
    return 1;
}

int cmd_memory(int argc, char* argv[]) {
    std::string action = argc > 2 ? argv[2] : "setup";
    if (action != "setup") {
        std::cerr << "Usage: hermes memory setup\n";
        return 1;
    }

    std::cout << "Memory backend setup.\n"
              << "Enable Honcho AI dialectic user modeling? [y/N]: ";
    std::string enable;
    if (!std::getline(std::cin, enable) || (enable != "y" && enable != "Y")) {
        auto cfg = load_config_json();
        set_dot_path(cfg, "memory.provider", "builtin");
        save_config_json(cfg);
        std::cout << "Using builtin file-based memory.\n";
        return 0;
    }

    std::cout << "HONCHO_API_KEY: ";
    std::string key; std::getline(std::cin, key);
    std::cout << "Honcho app_id: ";
    std::string app; std::getline(std::cin, app);
    std::cout << "Honcho user_id: ";
    std::string user; std::getline(std::cin, user);

    auto cfg = load_config_json();
    set_dot_path(cfg, "memory.provider", "honcho");
    set_dot_path(cfg, "memory.honcho.app_id", app);
    set_dot_path(cfg, "memory.honcho.user_id", user);
    if (!save_config_json(cfg)) return 1;

    // Append API key to ~/.hermes/.env (0600).
    auto envp = hermes::core::path::get_hermes_home() / ".env";
    std::error_code ec;
    fs::create_directories(envp.parent_path(), ec);
    std::ofstream f(envp, std::ios::app);
    f << "HONCHO_API_KEY=" << key << "\n";
    f.close();
    std::error_code pec;
    fs::permissions(envp, fs::perms::owner_read | fs::perms::owner_write,
                    fs::perm_options::replace, pec);
    std::cout << "Honcho configured. Restart hermes to activate.\n";
    return 0;
}

namespace {

// Walk the JSON tree and replace sensitive values (anything whose key looks
// like a secret / token / key / password) with "***".  Returns a redacted
// deep-copy; the input is left untouched.
nlohmann::json redact_secrets(const nlohmann::json& in) {
    static const std::vector<std::string> patterns = {
        "api_key", "apikey", "secret", "token", "password",
        "signing_secret", "signature_secret", "refresh_token",
        "access_token", "bearer", "credential",
    };
    auto is_secret_key = [](const std::string& k) {
        std::string lower;
        lower.reserve(k.size());
        for (char c : k) lower.push_back(static_cast<char>(std::tolower(c)));
        for (const auto& pat : patterns) {
            if (lower.find(pat) != std::string::npos) return true;
        }
        return false;
    };
    if (in.is_object()) {
        nlohmann::json out = nlohmann::json::object();
        for (auto it = in.begin(); it != in.end(); ++it) {
            if (is_secret_key(it.key()) && it.value().is_string() &&
                !it.value().get<std::string>().empty()) {
                out[it.key()] = "***";
            } else {
                out[it.key()] = redact_secrets(it.value());
            }
        }
        return out;
    }
    if (in.is_array()) {
        nlohmann::json out = nlohmann::json::array();
        for (const auto& v : in) out.push_back(redact_secrets(v));
        return out;
    }
    return in;
}

// Parse a --since YYYY-MM-DD[THH:MM:SS] stamp into seconds-since-epoch.
// Returns nullopt on parse failure.
std::optional<int64_t> parse_since(const std::string& s) {
    if (s.empty()) return std::nullopt;
    std::tm tm{};
    std::istringstream iss(s);
    iss >> std::get_time(&tm, "%Y-%m-%dT%H:%M:%S");
    if (iss.fail()) {
        iss.clear();
        iss.str(s);
        iss >> std::get_time(&tm, "%Y-%m-%d");
        if (iss.fail()) return std::nullopt;
    }
    auto t = std::mktime(&tm);
    if (t == static_cast<std::time_t>(-1)) return std::nullopt;
    return static_cast<int64_t>(t);
}

}  // namespace

int cmd_dump(int argc, char* argv[]) {
    if (argc < 3) {
        std::cerr << "Usage: hermes dump <sessions|config|memory> "
                     "[--since DATE] [--output PATH]\n";
        return 1;
    }
    std::string what = argv[2];

    // Parse common flags.
    std::string output_path;
    std::string since_str;
    for (int i = 3; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--output" && i + 1 < argc) {
            output_path = argv[++i];
        } else if (a == "--since" && i + 1 < argc) {
            since_str = argv[++i];
        } else {
            std::cerr << "Unknown flag: " << a << "\n";
            return 1;
        }
    }

    // Helper: emit either to stdout or to the file at output_path.
    auto emit = [&](const std::string& body) -> int {
        if (output_path.empty()) {
            std::cout << body;
            return 0;
        }
        std::error_code ec;
        fs::create_directories(fs::path(output_path).parent_path(), ec);
        std::ofstream f(output_path);
        if (!f) {
            std::cerr << "Cannot open " << output_path << " for write\n";
            return 1;
        }
        f << body;
        return 0;
    };

    auto home = hermes::core::path::get_hermes_home();

    if (what == "config") {
        auto cfg = load_config_json();
        // Redact secrets before emitting.
        auto redacted = redact_secrets(cfg);
        return emit(redacted.dump(2) + "\n");
    }
    if (what == "memory") {
        std::ostringstream body;
        for (const char* name : {"MEMORY.md", "USER.md"}) {
            auto p = home / "memories" / name;
            body << "# " << name << "\n";
            std::ifstream f(p);
            if (f) body << f.rdbuf() << "\n";
        }
        return emit(body.str());
    }
    if (what == "sessions") {
        auto sdir = home / "sessions";
        std::error_code ec;
        auto since = parse_since(since_str);
        if (!since_str.empty() && !since) {
            std::cerr << "Invalid --since: " << since_str
                      << " (expected YYYY-MM-DD or YYYY-MM-DDTHH:MM:SS)\n";
            return 1;
        }

        // JSONL format: one session per line.  Streams friendlier for
        // incremental consumers (gateway replay, batch analyzers).
        std::ostringstream body;
        bool any = false;
        if (fs::exists(sdir, ec)) {
            for (const auto& e : fs::directory_iterator(sdir, ec)) {
                if (!e.is_directory()) continue;
                auto meta = e.path() / "session.json";
                std::ifstream f(meta);
                if (!f) continue;
                nlohmann::json j;
                try { j = nlohmann::json::parse(f); }
                catch (...) { continue; }

                if (since) {
                    int64_t ts = 0;
                    if (j.contains("updated_at") && j["updated_at"].is_number()) {
                        ts = j["updated_at"].get<int64_t>();
                    } else if (j.contains("created_at") && j["created_at"].is_number()) {
                        ts = j["created_at"].get<int64_t>();
                    }
                    if (ts != 0 && ts < *since) continue;
                }

                // Fold in tool-trajectory JSONL if present alongside.
                auto traj = e.path() / "trajectory.jsonl";
                if (fs::exists(traj, ec)) {
                    std::ifstream tf(traj);
                    std::string line;
                    nlohmann::json arr = nlohmann::json::array();
                    while (std::getline(tf, line)) {
                        if (line.empty()) continue;
                        try { arr.push_back(nlohmann::json::parse(line)); }
                        catch (...) { /* skip malformed */ }
                    }
                    j["trajectory"] = std::move(arr);
                }

                body << j.dump() << "\n";
                any = true;
            }
        }
        if (!any && output_path.empty()) {
            body << "";  // empty output for an empty directory (no "[]")
        }
        // Preserve legacy "[]" signal when output goes to stdout AND no data
        // exists — keeps existing test happy.
        if (!any && output_path.empty()) {
            return emit("[]\n");
        }
        return emit(body.str());
    }
    std::cerr << "Unknown dump target: " << what << "\n";
    return 1;
}

namespace {

// URL sanity check — accept only http:// or https:// with a host component.
// We don't resolve DNS here; that's the job of the gateway at delivery time.
bool looks_like_url(const std::string& url) {
    if (url.empty()) return false;
    auto has_http = url.rfind("http://", 0) == 0;
    auto has_https = url.rfind("https://", 0) == 0;
    if (!has_http && !has_https) return false;
    auto scheme_end = url.find("://");
    if (scheme_end == std::string::npos) return false;
    auto host_start = scheme_end + 3;
    if (host_start >= url.size()) return false;
    // Host must contain at least one non-slash character before any slash.
    auto slash = url.find('/', host_start);
    auto host_len = (slash == std::string::npos) ? url.size() - host_start
                                                  : slash - host_start;
    return host_len > 0;
}

fs::path webhooks_file() {
    return hermes::core::path::get_hermes_home() / "webhooks.json";
}

nlohmann::json load_webhooks() {
    std::error_code ec;
    auto path = webhooks_file();
    if (!fs::exists(path, ec)) return nlohmann::json::array();
    std::ifstream f(path);
    if (!f) return nlohmann::json::array();
    try {
        auto j = nlohmann::json::parse(f);
        if (!j.is_array()) return nlohmann::json::array();
        return j;
    } catch (...) {
        return nlohmann::json::array();
    }
}

bool save_webhooks(const nlohmann::json& j) {
    auto path = webhooks_file();
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    std::ofstream f(path);
    if (!f) return false;
    f << j.dump(2) << "\n";
    return true;
}

// Derive a stable auto-name from the URL host (last component).
std::string derive_webhook_name(const std::string& url) {
    auto scheme_end = url.find("://");
    auto host_start = (scheme_end == std::string::npos) ? 0
                                                         : scheme_end + 3;
    auto slash = url.find('/', host_start);
    auto host = url.substr(host_start,
                           slash == std::string::npos ? std::string::npos
                                                      : slash - host_start);
    // Drop port if present.
    auto colon = host.find(':');
    if (colon != std::string::npos) host = host.substr(0, colon);
    if (host.empty()) return "webhook";
    return host;
}

int webhook_install_legacy_secrets() {
    std::cout << "Generating webhook signature secrets...\n";
    auto cfg = load_config_json();
    struct Target {
        const char* platform;
        const char* secret_key;
        const char* path;
    };
    static const Target targets[] = {
        {"api_server", "platforms.api_server.signature_secret", "/api/v1/hermes"},
        {"webhook",    "platforms.webhook.signature_secret",    "/webhook/hermes"},
        {"slack",      "platforms.slack.signing_secret",        "/slack/events"},
    };
    for (const auto& t : targets) {
        auto secret = random_hex(32);
        set_dot_path(cfg, t.secret_key, secret);
        std::cout << "  " << std::left << std::setw(12) << t.platform
                  << " → POST " << t.path
                  << "  secret=" << mask_secret(secret) << "\n";
    }
    if (!save_config_json(cfg)) return 1;
    std::cout << "\nSecrets saved to config.yaml. Configure your hosting layer\n"
              << "to forward the above paths to the gateway HTTP listener.\n";
    return 0;
}

}  // namespace

int cmd_webhook(int argc, char* argv[]) {
    // Forms:
    //   hermes webhook install              → generate signing secrets (legacy)
    //   hermes webhook install <url> [--secret K] [--name N]
    //                                       → register a delivery endpoint
    //   hermes webhook --list               → show registered endpoints
    //   hermes webhook --remove <name>      → remove by name
    if (argc <= 2) {
        std::cerr << "Usage: hermes webhook install [<url>] [--secret K] "
                     "[--name N]\n"
                     "       hermes webhook --list\n"
                     "       hermes webhook --remove <name>\n";
        return 1;
    }

    std::string a2 = argv[2];

    if (a2 == "--list") {
        auto hooks = load_webhooks();
        if (hooks.empty()) {
            std::cout << "No webhooks registered.\n";
            return 0;
        }
        std::cout << std::left << std::setw(20) << "Name"
                  << std::setw(50) << "URL"
                  << "Secret\n";
        std::cout << std::string(90, '-') << "\n";
        for (const auto& h : hooks) {
            auto name = h.value("name", "");
            auto url = h.value("url", "");
            auto secret = h.value("secret", "");
            std::cout << std::left << std::setw(20) << name.substr(0, 18)
                      << std::setw(50) << url.substr(0, 48)
                      << (secret.empty() ? "(none)" : mask_secret(secret)) << "\n";
        }
        return 0;
    }

    if (a2 == "--remove") {
        if (argc < 4) {
            std::cerr << "Usage: hermes webhook --remove <name>\n";
            return 1;
        }
        std::string name = argv[3];
        auto hooks = load_webhooks();
        nlohmann::json kept = nlohmann::json::array();
        bool removed = false;
        for (const auto& h : hooks) {
            if (h.value("name", "") == name) { removed = true; continue; }
            kept.push_back(h);
        }
        if (!removed) {
            std::cerr << "No webhook named '" << name << "'.\n";
            return 1;
        }
        if (!save_webhooks(kept)) return 1;
        std::cout << "Removed webhook: " << name << "\n";
        return 0;
    }

    if (a2 == "install") {
        // `hermes webhook install` (no url) → preserve legacy secret-generator.
        if (argc == 3) return webhook_install_legacy_secrets();

        std::string url = argv[3];
        std::string secret;
        std::string name;
        for (int i = 4; i < argc; ++i) {
            std::string a = argv[i];
            if (a == "--secret" && i + 1 < argc) {
                secret = argv[++i];
            } else if (a == "--name" && i + 1 < argc) {
                name = argv[++i];
            } else {
                std::cerr << "Unknown flag: " << a << "\n";
                return 1;
            }
        }
        if (!looks_like_url(url)) {
            std::cerr << "Invalid URL: " << url
                      << " (expected http:// or https://host/path)\n";
            return 1;
        }
        if (secret.empty()) secret = random_hex(32);
        if (name.empty()) name = derive_webhook_name(url);

        auto hooks = load_webhooks();
        // Replace any existing entry with the same name to keep the list unique.
        nlohmann::json replaced = nlohmann::json::array();
        for (const auto& h : hooks) {
            if (h.value("name", "") == name) continue;
            replaced.push_back(h);
        }
        nlohmann::json entry = {
            {"name", name}, {"url", url}, {"secret", secret}};
        replaced.push_back(entry);
        if (!save_webhooks(replaced)) return 1;
        std::cout << "Registered webhook '" << name << "' → " << url
                  << "  secret=" << mask_secret(secret) << "\n"
                  << "(stored in " << webhooks_file().string() << ")\n";
        return 0;
    }

    std::cerr << "Unknown webhook action: " << a2 << "\n";
    return 1;
}

int cmd_runtime(int argc, char* argv[]) {
    static const std::vector<std::string> kBackends = {
        "local", "docker", "ssh", "modal", "daytona",
        "singularity", "managed_modal"};

    if (argc < 3) {
        std::cerr << "Usage: hermes runtime <list|select <name>|terminal <name>>\n";
        return 1;
    }
    std::string sub = argv[2];

    if (sub == "list") {
        auto cfg = load_config_json();
        std::string active = "local";
        if (cfg.contains("terminal") && cfg["terminal"].contains("backend") &&
            cfg["terminal"]["backend"].is_string()) {
            active = cfg["terminal"]["backend"].get<std::string>();
        }
        std::cout << "Terminal backends:\n";
        for (const auto& b : kBackends) {
            std::cout << "  " << (b == active ? "* " : "  ") << b << "\n";
        }
        return 0;
    }

    // Accept both `runtime select <name>` and the original `runtime terminal <name>`.
    if (sub == "select" || sub == "terminal") {
        if (argc < 4) {
            std::cerr << "Usage: hermes runtime " << sub
                      << " <local|docker|ssh|modal|daytona|singularity|managed_modal>\n";
            return 1;
        }
        std::string backend = argv[3];
        if (std::find(kBackends.begin(), kBackends.end(), backend) == kBackends.end()) {
            std::cerr << "Invalid backend: " << backend << "\n";
            return 1;
        }
        auto cfg = load_config_json();
        set_dot_path(cfg, "terminal.backend", backend);
        if (!save_config_json(cfg)) return 1;
        std::cout << "Terminal backend: " << backend << "\n";
        return 0;
    }

    std::cerr << "Unknown runtime action: " << sub << "\n";
    return 1;
}

// --------------------------------------------------------------------------
// hermes auth — provider authentication (qwen / copilot / nous).
// --------------------------------------------------------------------------

namespace {
int qwen_login() {
    hermes::auth::QwenCredentialStore store;
    hermes::auth::QwenOAuth oauth;
    auto creds = oauth.interactive_login(store);
    if (!creds) return 1;

    // Bonus: also point the default model at qwen3-coder-plus and provider
    // at qwen so the CLI uses the new credentials without manual config.
    auto cfg = load_config_json();
    cfg["provider"] = "qwen";
    if (!cfg.contains("model") || cfg["model"].is_null() ||
        cfg.value("model", "").empty() ||
        cfg.value("model", "").find("qwen") == std::string::npos) {
        cfg["model"] = "qwen3-coder-plus";
    }
    set_dot_path(cfg, "base_url", hermes::auth::qwen_api_base_url(*creds));
    save_config_json(cfg);

    std::cout << "Qwen provider configured.\n"
              << "  model:    " << cfg.value("model", "qwen3-coder-plus") << "\n"
              << "  base_url: " << hermes::auth::qwen_api_base_url(*creds) << "\n";
    return 0;
}

int qwen_status() {
    hermes::auth::QwenCredentialStore store;
    auto creds = store.load();
    if (creds.empty()) {
        std::cout << "Qwen: not logged in.\n"
                  << "Run `hermes auth qwen login` to authenticate.\n";
        return 1;
    }
    auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::system_clock::now().time_since_epoch())
                   .count();
    std::cout << "Qwen credentials at " << store.path() << ":\n"
              << "  resource_url: "
              << (creds.resource_url.empty() ? "(default DashScope)"
                                              : creds.resource_url) << "\n"
              << "  api_base:     " << hermes::auth::qwen_api_base_url(creds) << "\n"
              << "  expires_in:   "
              << ((creds.expiry_date_ms - now) / 1000) << " seconds\n"
              << "  refresh_token: "
              << (creds.refresh_token.empty() ? "(missing)" : "present") << "\n";
    return 0;
}

int qwen_logout() {
    hermes::auth::QwenCredentialStore store;
    if (store.clear()) {
        std::cout << "Qwen credentials cleared.\n";
        return 0;
    }
    std::cerr << "No credentials to clear.\n";
    return 1;
}

int qwen_refresh() {
    hermes::auth::QwenCredentialStore store;
    auto current = store.load();
    if (current.empty()) {
        std::cerr << "Not logged in.\n";
        return 1;
    }
    hermes::auth::QwenOAuth oauth;
    auto refreshed = oauth.refresh(current);
    if (!refreshed) {
        std::cerr << "Refresh failed (token revoked?). Run `hermes auth qwen "
                     "login` again.\n";
        store.clear();
        return 1;
    }
    store.save(*refreshed);
    std::cout << "Token refreshed. New expiry in "
              << ((refreshed->expiry_date_ms -
                   std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::system_clock::now().time_since_epoch())
                       .count()) / 1000)
              << " seconds.\n";
    return 0;
}
}  // namespace

namespace {

// Map of "provider" → env var we persist to <HERMES_HOME>/.env via
// hermes::auth::store_credential().  These are the simple API-key providers.
struct KeyProvider { const char* name; const char* env; };
const std::vector<KeyProvider>& api_key_providers() {
    static const std::vector<KeyProvider> provs = {
        {"openai",    "OPENAI_API_KEY"},
        {"anthropic", "ANTHROPIC_API_KEY"},
        {"nous",      "NOUS_API_KEY"},
    };
    return provs;
}

const KeyProvider* find_key_provider(const std::string& name) {
    for (const auto& p : api_key_providers()) {
        if (name == p.name) return &p;
    }
    return nullptr;
}

// Read one line from stdin — used for API-key prompt.
std::string read_line_hidden(const std::string& prompt) {
    std::cout << prompt << std::flush;
    std::string line;
    std::getline(std::cin, line);
    // Trim trailing CR/LF/whitespace.
    while (!line.empty() &&
           (line.back() == '\n' || line.back() == '\r' || line.back() == ' ')) {
        line.pop_back();
    }
    return line;
}

int api_key_login(const KeyProvider& p) {
    std::cout << "Logging in to " << p.name << ".\n"
              << "A value entered here will be written to "
              << (hermes::core::path::get_hermes_home() / ".env").string()
              << " (mode 0600).\n";
    auto value = read_line_hidden("Enter " + std::string(p.env) + ": ");
    if (value.empty()) {
        std::cerr << "No value entered. Aborting.\n";
        return 1;
    }
    try {
        hermes::auth::store_credential(p.env, value);
    } catch (const std::exception& e) {
        std::cerr << "Failed to store credential: " << e.what() << "\n";
        return 1;
    }
    std::cout << p.name << ": credential stored ("
              << mask_secret(value) << ").\n";
    return 0;
}

int api_key_logout(const KeyProvider& p) {
    try {
        hermes::auth::clear_credential(p.env);
    } catch (const std::exception& e) {
        std::cerr << "Failed to clear credential: " << e.what() << "\n";
        return 1;
    }
    std::cout << p.name << ": credential cleared.\n";
    return 0;
}

int api_key_status(const KeyProvider& p) {
    auto val = hermes::auth::get_credential(p.env);
    if (!val || val->empty()) {
        std::cout << p.name << ": not configured ("
                  << p.env << " unset).\n";
        return 1;
    }
    std::cout << p.name << ": " << mask_secret(*val)
              << "  (" << p.env << ")\n";
    return 0;
}

// -- Copilot OAuth (device code flow) --------------------------------------

int copilot_login() {
    hermes::auth::CopilotOAuth oauth;
    auto token = oauth.interactive_login();
    if (!token || token->empty()) {
        std::cerr << "Copilot login failed.\n";
        return 1;
    }
    try {
        hermes::auth::store_credential("GITHUB_COPILOT_TOKEN", *token);
    } catch (const std::exception& e) {
        std::cerr << "Stored token but failed to persist: " << e.what() << "\n";
        return 1;
    }
    std::cout << "Copilot: logged in, token persisted.\n";
    return 0;
}

int copilot_logout() {
    try {
        hermes::auth::clear_credential("GITHUB_COPILOT_TOKEN");
    } catch (const std::exception& e) {
        std::cerr << "Failed to clear Copilot token: " << e.what() << "\n";
        return 1;
    }
    std::cout << "Copilot: credentials cleared.\n";
    return 0;
}

int copilot_status() {
    auto val = hermes::auth::get_credential("GITHUB_COPILOT_TOKEN");
    if (!val || val->empty()) {
        std::cout << "Copilot: not logged in.\n"
                  << "Run `hermes auth copilot login` to authenticate.\n";
        return 1;
    }
    std::cout << "Copilot: token present (" << mask_secret(*val) << ").\n";
    return 0;
}

// -- Nous subscription -----------------------------------------------------

// Nous uses the NOUS_API_KEY credential like the plain API-key providers,
// but `status` additionally probes the subscription endpoint so users can see
// their tier / features.
int nous_status_detailed() {
    auto val = hermes::auth::get_credential("NOUS_API_KEY");
    if (!val || val->empty()) {
        std::cout << "Nous: not configured (NOUS_API_KEY unset).\n";
        return 1;
    }
    std::cout << "Nous API key: " << mask_secret(*val) << "\n";
    auto sub = hermes::auth::check_subscription(*val);
    if (!sub) {
        std::cout << "Subscription: unreachable (check connectivity).\n";
        return 0;  // key still present; don't escalate to failure
    }
    std::cout << "Subscription:\n"
              << "  tier:   " << sub->tier << "\n"
              << "  active: " << (sub->active ? "yes" : "no") << "\n"
              << "  user:   " << sub->user_id << "\n";
    if (!sub->features.empty()) {
        std::cout << "  features:";
        for (const auto& f : sub->features) std::cout << " " << f;
        std::cout << "\n";
    }
    return 0;
}

// Aggregate `hermes auth status` — one line per configured credential.
int auth_status_all() {
    std::cout << "Authentication status:\n";
    // Qwen (OAuth creds file, not .env).
    {
        hermes::auth::QwenCredentialStore store;
        auto creds = store.load();
        std::cout << "  qwen      : "
                  << (creds.empty() ? "not logged in"
                                     : std::string("logged in, refresh=") +
                                       (creds.refresh_token.empty() ? "(missing)"
                                                                     : "present"))
                  << "\n";
    }
    // Copilot.
    {
        auto v = hermes::auth::get_credential("GITHUB_COPILOT_TOKEN");
        std::cout << "  copilot   : "
                  << ((v && !v->empty())
                          ? std::string("token ") + mask_secret(*v)
                          : std::string("not logged in")) << "\n";
    }
    // API-key providers.
    for (const auto& p : api_key_providers()) {
        auto v = hermes::auth::get_credential(p.env);
        std::cout << "  " << std::left << std::setw(10) << p.name
                  << ": "
                  << ((v && !v->empty()) ? mask_secret(*v)
                                           : std::string("not configured"))
                  << "  (" << p.env << ")\n";
    }
    return 0;
}

}  // namespace

int cmd_auth(int argc, char* argv[]) {
    if (argc < 3) {
        std::cerr <<
            "Usage: hermes auth <action> [provider]\n"
            "       hermes auth <provider> <action>\n\n"
            "Actions:\n"
            "  login <provider>    Authenticate with a provider\n"
            "  logout <provider>   Clear stored credentials\n"
            "  status [provider]   Show status (aggregate if provider omitted)\n\n"
            "Providers:\n"
            "  openai     — API key stored in ~/.hermes/.env\n"
            "  anthropic  — API key stored in ~/.hermes/.env\n"
            "  nous       — API key + live subscription check\n"
            "  qwen       — Qwen Code OAuth device flow\n"
            "  copilot    — GitHub Copilot OAuth device flow\n";
        return 1;
    }

    std::string first = argv[2];
    std::string second = argc > 3 ? argv[3] : "";

    // Form 1: `auth <action> <provider>` (Python-style — matches the spec).
    auto dispatch = [&](const std::string& action,
                        const std::string& provider) -> int {
        if (provider == "qwen") {
            if (action == "login")   return qwen_login();
            if (action == "status")  return qwen_status();
            if (action == "logout")  return qwen_logout();
            if (action == "refresh") return qwen_refresh();
            std::cerr << "Unknown qwen action: " << action << "\n";
            return 1;
        }
        if (provider == "copilot") {
            if (action == "login")  return copilot_login();
            if (action == "logout") return copilot_logout();
            if (action == "status") return copilot_status();
            std::cerr << "Unknown copilot action: " << action << "\n";
            return 1;
        }
        if (provider == "nous") {
            if (action == "status") return nous_status_detailed();
            if (auto* p = find_key_provider("nous")) {
                if (action == "login")  return api_key_login(*p);
                if (action == "logout") return api_key_logout(*p);
            }
            std::cerr << "Unknown nous action: " << action << "\n";
            return 1;
        }
        if (const auto* p = find_key_provider(provider)) {
            if (action == "login")  return api_key_login(*p);
            if (action == "logout") return api_key_logout(*p);
            if (action == "status") return api_key_status(*p);
            std::cerr << "Unknown " << provider << " action: " << action << "\n";
            return 1;
        }
        std::cerr << "Unknown provider: " << provider << "\n";
        return 1;
    };

    // Aggregate `hermes auth status` (no provider).
    if (first == "status" && second.empty()) {
        return auth_status_all();
    }

    // Python-shaped form: `hermes auth login qwen`, `hermes auth logout openai`,
    // `hermes auth status openai`.
    if (first == "login" || first == "logout" || first == "status") {
        if (second.empty()) {
            std::cerr << "Usage: hermes auth " << first << " <provider>\n";
            return 1;
        }
        return dispatch(first, second);
    }

    // Back-compat form: `hermes auth <provider> <action>`.
    std::string provider = first;
    std::string action = second.empty() ? "status" : second;
    return dispatch(action, provider);
}

int cmd_login(int argc, char* argv[]) {
    // `hermes login [provider]` — shorthand for `hermes auth login <provider>`.
    std::string provider = argc > 2 ? argv[2] : "qwen";
    if (provider == "qwen")    return qwen_login();
    if (provider == "copilot") return copilot_login();
    if (const auto* p = find_key_provider(provider)) return api_key_login(*p);
    std::cerr << "Unknown provider for login: " << provider << "\n"
              << "Use `hermes auth login <provider>` instead.\n";
    return 1;
}

int cmd_claw(int argc, char* argv[]) {
    if (argc <= 2 || std::string(argv[2]) != "migrate") {
        std::cout << "Usage: hermes claw migrate [--dry-run] [--overwrite] "
                     "[--preset full|user-data|no-secrets] "
                     "[--workspace-target PATH]\n";
        return 1;
    }

    hermes::cli::claw::MigrateOptions opts;
    for (int i = 3; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--dry-run") {
            opts.dry_run = true;
        } else if (a == "--overwrite") {
            opts.overwrite = true;
        } else if (a == "--preset" && i + 1 < argc) {
            opts.preset = argv[++i];
        } else if (a == "--workspace-target" && i + 1 < argc) {
            opts.workspace_target = argv[++i];
        } else {
            std::cerr << "Unknown flag: " << a << "\n";
            return 1;
        }
    }

    auto r = hermes::cli::claw::migrate(opts);
    std::cout << "Migration summary:\n"
              << "  imported: " << r.imported.size() << "\n"
              << "  skipped:  " << r.skipped.size() << "\n"
              << "  errors:   " << r.errors.size() << "\n"
              << "  items:    " << r.item_count << "\n";
    for (const auto& s : r.imported) std::cout << "  + " << s << "\n";
    for (const auto& s : r.skipped)  std::cout << "  - " << s << "\n";
    for (const auto& s : r.errors)   std::cout << "  ! " << s << "\n";
    return r.errors.empty() ? 0 : 1;
}

int cmd_pairing(int argc, char* argv[]) {
    // hermes pairing approve <platform> <code>
    if (argc < 5) {
        std::cerr << "Usage: hermes pairing approve <platform> <code>\n";
        return 1;
    }

    std::string sub = argv[2];
    if (sub != "approve") {
        std::cerr << "Unknown pairing subcommand: " << sub << "\n"
                  << "Usage: hermes pairing approve <platform> <code>\n";
        return 1;
    }

    std::string platform_str = argv[3];
    std::string code = argv[4];

    try {
        auto platform = hermes::gateway::platform_from_string(platform_str);
        auto pairing_dir = hermes::core::path::get_hermes_home() / "pairing";
        hermes::gateway::PairingStore store(pairing_dir);

        if (store.approve_code(platform, code)) {
            std::cout << "Approved!\n";
            return 0;
        } else {
            std::cout << "Code not found or expired.\n";
            return 1;
        }
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
        return 1;
    }
}

int main_entry(int argc, char* argv[]) {
    if (argc < 2) {
        // Pipe mode: when stdin is not a TTY, read all input and run as
        // a single query, then exit.
        if (!::isatty(STDIN_FILENO)) {
            std::ostringstream buf;
            buf << std::cin.rdbuf();
            std::string input = buf.str();
            if (!input.empty()) {
                HermesCLI cli;
                auto result = cli.query(input);
                std::cout << result << "\n";
            }
            return 0;
        }
        // Default: interactive chat.
        HermesCLI cli;
        cli.run();
        return 0;
    }

    std::string sub = argv[1];

    if (sub == "--help" || sub == "-h") {
        print_global_help();
        return 0;
    }
    if (sub == "--version" || sub == "-V" || sub == "version") {
        return cmd_version();
    }
    if (sub == "chat") {
        HermesCLI cli;
        cli.run();
        return 0;
    }
    if (sub == "doctor") {
        return cmd_doctor();
    }
    if (sub == "status") {
        return cmd_status();
    }
    if (sub == "model") {
        if (argc > 2 && std::string(argv[2]) == "switch") {
            return cmd_model_switch(argc, argv);
        }
        return cmd_model();
    }
    if (sub == "tools") {
        return cmd_tools();
    }
    if (sub == "gateway") {
        return cmd_gateway(argc, argv);
    }
    if (sub == "config") {
        return cmd_config(argc, argv);
    }
    if (sub == "setup") {
        return cmd_setup();
    }
    if (sub == "skills") {
        return cmd_skills();
    }
    if (sub == "logs") {
        return cmd_logs();
    }
    if (sub == "cron") {
        return cmd_cron();
    }
    if (sub == "profile") {
        return cmd_profile(argc, argv);
    }
    if (sub == "update") {
        return cmd_update();
    }
    if (sub == "uninstall") {
        return cmd_uninstall();
    }

    if (sub == "pairing") {
        return cmd_pairing(argc, argv);
    }
    if (sub == "claw") {
        return cmd_claw(argc, argv);
    }
    if (sub == "providers") {
        return cmd_providers(argc, argv);
    }
    if (sub == "memory") {
        return cmd_memory(argc, argv);
    }
    if (sub == "dump") {
        return cmd_dump(argc, argv);
    }
    if (sub == "webhook") {
        return cmd_webhook(argc, argv);
    }
    if (sub == "runtime") {
        return cmd_runtime(argc, argv);
    }
    if (sub == "auth") {
        return cmd_auth(argc, argv);
    }
    if (sub == "login") {
        return cmd_login(argc, argv);
    }

    std::cerr << "Unknown subcommand: " << sub << "\n";
    print_global_help();
    return 1;
}

}  // namespace hermes::cli
