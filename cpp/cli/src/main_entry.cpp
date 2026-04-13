#include "hermes/cli/main_entry.hpp"

#include "hermes/cli/claw_migrate.hpp"
#include "hermes/cli/hermes_cli.hpp"
#include "hermes/config/loader.hpp"
#include "hermes/core/path.hpp"
#include "hermes/cron/jobs.hpp"
#include "hermes/gateway/pairing.hpp"
#include "hermes/gateway/gateway_config.hpp"
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
    std::string old_model = cfg.value("model", "(unset)");
    cfg["model"] = new_model;
    if (!save_config_json(cfg)) return 1;
    std::cout << "Switched model: " << old_model << " → " << new_model << "\n";
    return 0;
}

int cmd_providers(int argc, char* argv[]) {
    std::string action = argc > 2 ? argv[2] : "list";
    if (action != "list") {
        std::cerr << "Usage: hermes providers list\n";
        return 1;
    }

    struct Prov { const char* name; const char* env; };
    static const Prov provs[] = {
        {"OpenRouter", "OPENROUTER_API_KEY"},
        {"Anthropic",  "ANTHROPIC_API_KEY"},
        {"OpenAI",     "OPENAI_API_KEY"},
        {"Google",     "GOOGLE_API_KEY"},
        {"Mistral",    "MISTRAL_API_KEY"},
        {"MiniMax",    "MINIMAX_API_KEY"},
        {"Groq",       "GROQ_API_KEY"},
        {"Nous",       "NOUS_API_KEY"},
        {"Firecrawl",  "FIRECRAWL_API_KEY"},
        {"Exa",        "EXA_API_KEY"},
        {"Tavily",     "TAVILY_API_KEY"},
        {"ElevenLabs", "ELEVENLABS_API_KEY"},
    };

    std::cout << std::left << std::setw(14) << "Provider"
              << std::setw(26) << "Env Var"
              << "Status\n";
    std::cout << std::string(60, '-') << "\n";
    for (const auto& p : provs) {
        const char* val = std::getenv(p.env);
        std::string status = val ? mask_secret(val) : "(unset)";
        std::cout << std::left << std::setw(14) << p.name
                  << std::setw(26) << p.env
                  << status << "\n";
    }
    return 0;
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

int cmd_dump(int argc, char* argv[]) {
    if (argc < 3) {
        std::cerr << "Usage: hermes dump <sessions|config|memory>\n";
        return 1;
    }
    std::string what = argv[2];
    auto home = hermes::core::path::get_hermes_home();

    if (what == "config") {
        std::cout << load_config_json().dump(2) << "\n";
        return 0;
    }
    if (what == "memory") {
        for (const char* name : {"MEMORY.md", "USER.md"}) {
            auto p = home / "memories" / name;
            std::cout << "# " << name << "\n";
            std::ifstream f(p);
            if (f) std::cout << f.rdbuf() << "\n";
        }
        return 0;
    }
    if (what == "sessions") {
        auto sdir = home / "sessions";
        std::error_code ec;
        if (!fs::exists(sdir, ec)) {
            std::cout << "[]\n";
            return 0;
        }
        nlohmann::json arr = nlohmann::json::array();
        for (const auto& e : fs::directory_iterator(sdir, ec)) {
            if (!e.is_directory()) continue;
            auto meta = e.path() / "session.json";
            std::ifstream f(meta);
            if (f) {
                try { arr.push_back(nlohmann::json::parse(f)); }
                catch (...) { /* skip */ }
            }
        }
        std::cout << arr.dump(2) << "\n";
        return 0;
    }
    std::cerr << "Unknown dump target: " << what << "\n";
    return 1;
}

int cmd_webhook(int argc, char* argv[]) {
    std::string action = argc > 2 ? argv[2] : "install";
    if (action != "install") {
        std::cerr << "Usage: hermes webhook install\n";
        return 1;
    }

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

int cmd_runtime(int argc, char* argv[]) {
    if (argc < 4) {
        std::cerr << "Usage: hermes runtime terminal <local|docker|ssh|modal|"
                     "daytona|singularity|managed_modal>\n";
        return 1;
    }
    std::string sub = argv[2];
    if (sub != "terminal") {
        std::cerr << "Unknown runtime action: " << sub << "\n";
        return 1;
    }
    std::string backend = argv[3];
    static const std::vector<std::string> valid = {
        "local", "docker", "ssh", "modal", "daytona",
        "singularity", "managed_modal"};
    if (std::find(valid.begin(), valid.end(), backend) == valid.end()) {
        std::cerr << "Invalid backend: " << backend << "\n";
        return 1;
    }
    auto cfg = load_config_json();
    set_dot_path(cfg, "terminal.backend", backend);
    if (!save_config_json(cfg)) return 1;
    std::cout << "Terminal backend: " << backend << "\n";
    return 0;
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

    std::cerr << "Unknown subcommand: " << sub << "\n";
    print_global_help();
    return 1;
}

}  // namespace hermes::cli
