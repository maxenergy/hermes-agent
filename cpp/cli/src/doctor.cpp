// Hermes doctor — full C++17 port of hermes_cli/doctor.py.
//
// This file contains the core diagnostic logic.  The public surface lives
// in hermes/cli/doctor.hpp; this implementation deliberately uses only
// cross-platform APIs plus a few POSIX hooks (guarded by `_WIN32`).

#include "hermes/cli/doctor.hpp"

#include "hermes/auth/credentials.hpp"
#include "hermes/config/loader.hpp"
#include "hermes/core/path.hpp"
#include "hermes/llm/llm_client.hpp"

#include <nlohmann/json.hpp>
#include <sqlite3.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string_view>
#include <system_error>
#include <utility>

#ifdef _WIN32
#  include <io.h>
#  define HERMES_ISATTY _isatty
#  define HERMES_FILENO _fileno
#else
#  include <sys/stat.h>
#  include <sys/types.h>
#  include <unistd.h>
#  include <signal.h>
#  define HERMES_ISATTY isatty
#  define HERMES_FILENO fileno
#endif

namespace hermes::cli::doctor {

namespace fs = std::filesystem;
using json = nlohmann::json;

// ── Color helpers ───────────────────────────────────────────────────────

namespace {

struct Palette {
    const char* green;
    const char* red;
    const char* yellow;
    const char* cyan;
    const char* dim;
    const char* bold;
    const char* reset;
};

Palette palette(bool color) {
    if (!color) return {"", "", "", "", "", "", ""};
    return {"\033[32m", "\033[31m", "\033[33m", "\033[36m",
            "\033[2m",  "\033[1m",  "\033[0m"};
}

const char* symbol(Severity s) {
    switch (s) {
        case Severity::Ok:   return "v";
        case Severity::Warn: return "!";
        case Severity::Fail: return "x";
        case Severity::Info: return "-";
    }
    return "?";
}

const char* sev_label(Severity s) {
    switch (s) {
        case Severity::Ok:   return "OK";
        case Severity::Warn: return "WARN";
        case Severity::Fail: return "FAIL";
        case Severity::Info: return "INFO";
    }
    return "?";
}

// Appends a row to the report and updates the counters.
void add_row(Report& r, std::string category, Severity sev, std::string label,
             std::string detail = "", std::string fix_hint = "") {
    Row row;
    row.category = std::move(category);
    row.severity = sev;
    row.label = std::move(label);
    row.detail = std::move(detail);
    row.fix_hint = std::move(fix_hint);
    switch (sev) {
        case Severity::Ok:   ++r.ok_count; break;
        case Severity::Warn: ++r.warn_count; break;
        case Severity::Fail: ++r.fail_count; break;
        default: break;
    }
    r.rows.push_back(std::move(row));
}

// Returns the resolved HERMES_HOME for this run — honours the test override.
fs::path resolve_home(const Options& opts) {
    if (!opts.home_override.empty()) return opts.home_override;
    return hermes::core::path::get_hermes_home();
}

// Reads an env var, falling back to the `.env` file in HERMES_HOME when the
// process environment is empty.  Mirrors the `dotenv.load_dotenv` call in
// the Python doctor.
std::string read_env_or_file(const std::string& key, const fs::path& home) {
    if (const char* v = std::getenv(key.c_str()); v && *v) return v;
    fs::path env_file = home / ".env";
    std::ifstream in(env_file);
    if (!in) return "";
    std::string line;
    while (std::getline(in, line)) {
        auto hash = line.find('#');
        if (hash != std::string::npos) line.resize(hash);
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string k = line.substr(0, eq);
        std::string v = line.substr(eq + 1);
        // trim
        auto ltrim = [](std::string& s) {
            s.erase(s.begin(), std::find_if(s.begin(), s.end(),
                [](unsigned char c) { return !std::isspace(c); }));
        };
        auto rtrim = [](std::string& s) {
            s.erase(std::find_if(s.rbegin(), s.rend(),
                [](unsigned char c) { return !std::isspace(c); }).base(), s.end());
        };
        ltrim(k); rtrim(k); ltrim(v); rtrim(v);
        if (v.size() >= 2 &&
            ((v.front() == '"' && v.back() == '"') ||
             (v.front() == '\'' && v.back() == '\''))) {
            v = v.substr(1, v.size() - 2);
        }
        if (k == key && !v.empty()) return v;
    }
    return "";
}

}  // namespace

// ── Pure helpers (exported) ─────────────────────────────────────────────

static constexpr std::array<const char*, 20> kProviderEnvHints = {
    "OPENROUTER_API_KEY", "OPENAI_API_KEY", "ANTHROPIC_API_KEY",
    "ANTHROPIC_TOKEN", "OPENAI_BASE_URL", "NOUS_API_KEY", "GLM_API_KEY",
    "ZAI_API_KEY", "Z_AI_API_KEY", "KIMI_API_KEY", "MINIMAX_API_KEY",
    "MINIMAX_CN_API_KEY", "KILOCODE_API_KEY", "DEEPSEEK_API_KEY",
    "DASHSCOPE_API_KEY", "HF_TOKEN", "AI_GATEWAY_API_KEY",
    "OPENCODE_ZEN_API_KEY", "OPENCODE_GO_API_KEY", "GOOGLE_API_KEY",
};

bool has_provider_env_config(const std::string& env_contents) {
    for (auto* k : kProviderEnvHints) {
        if (env_contents.find(k) != std::string::npos) return true;
    }
    return false;
}

Severity classify_http_status(int status_code, std::string& detail_out) {
    if (status_code == 200) {
        detail_out.clear();
        return Severity::Ok;
    }
    if (status_code == 401) {
        detail_out = "(invalid API key)";
        return Severity::Fail;
    }
    detail_out = "(HTTP " + std::to_string(status_code) + ")";
    if (status_code >= 400) return Severity::Warn;
    return Severity::Warn;
}

bool binary_on_path(const std::string& cmd) {
    const char* path_env = std::getenv("PATH");
    if (!path_env) return false;
    std::string p(path_env);
#ifdef _WIN32
    const char sep = ';';
    const std::vector<std::string> exts = {".exe", ".cmd", ".bat", ""};
#else
    const char sep = ':';
    const std::vector<std::string> exts = {""};
#endif
    std::size_t start = 0;
    while (start <= p.size()) {
        auto end = p.find(sep, start);
        if (end == std::string::npos) end = p.size();
        if (end > start) {
            fs::path dir(p.substr(start, end - start));
            for (const auto& ext : exts) {
                fs::path candidate = dir / (cmd + ext);
                std::error_code ec;
                if (fs::exists(candidate, ec) && !fs::is_directory(candidate, ec)) {
                    return true;
                }
            }
        }
        if (end == p.size()) break;
        start = end + 1;
    }
    return false;
}

std::uintmax_t available_disk_bytes(const fs::path& path) {
    std::error_code ec;
    auto info = fs::space(path, ec);
    if (ec) return 0;
    return info.available;
}

bool is_mode_0600(const fs::path& path) {
#ifdef _WIN32
    (void)path;
    return true;
#else
    struct stat st{};
    if (::stat(path.c_str(), &st) != 0) return false;
    return (st.st_mode & 0777) == 0600;
#endif
}

bool chmod_0600(const fs::path& path) {
#ifdef _WIN32
    (void)path;
    return true;
#else
    return ::chmod(path.c_str(), 0600) == 0;
#endif
}

bool fts5_available() {
    sqlite3* db = nullptr;
    if (sqlite3_open(":memory:", &db) != SQLITE_OK) {
        if (db) sqlite3_close(db);
        return false;
    }
    bool ok = (sqlite3_exec(db,
                  "CREATE VIRTUAL TABLE _fts_probe USING fts5(a);",
                  nullptr, nullptr, nullptr) == SQLITE_OK);
    sqlite3_close(db);
    return ok;
}

std::string sqlite_integrity_check(const fs::path& db_path) {
    sqlite3* db = nullptr;
    std::string path_str = db_path.string();
    if (sqlite3_open(path_str.c_str(), &db) != SQLITE_OK) {
        std::string err = sqlite3_errmsg(db);
        if (db) sqlite3_close(db);
        return err.empty() ? std::string("cannot open") : err;
    }
    std::string result;
    auto callback = +[](void* ud, int ncol, char** vals, char**) -> int {
        auto* out = static_cast<std::string*>(ud);
        if (ncol >= 1 && vals[0]) *out = vals[0];
        return 0;
    };
    char* errmsg = nullptr;
    int rc = sqlite3_exec(db, "PRAGMA integrity_check;", callback, &result, &errmsg);
    if (rc != SQLITE_OK) {
        std::string err = errmsg ? errmsg : "integrity check failed";
        if (errmsg) sqlite3_free(errmsg);
        sqlite3_close(db);
        return err;
    }
    sqlite3_close(db);
    if (result == "ok") return "";
    return result;
}

std::pair<bool, std::string> probe_mcp_child(const std::string& server_cmd) {
    // We just verify the executable portion of server_cmd is reachable.
    // Launching real MCP children during `doctor` would be invasive.
    std::istringstream iss(server_cmd);
    std::string first;
    iss >> first;
    if (first.empty()) return {false, "empty command"};
    // Absolute path? check file exists; else PATH lookup.
    std::error_code ec;
    if (first.find('/') != std::string::npos || first.find('\\') != std::string::npos) {
        if (!fs::exists(first, ec)) return {false, "binary not found: " + first};
    } else if (!binary_on_path(first)) {
        return {false, "binary not on PATH: " + first};
    }
    return {true, ""};
}

int read_skills_hub_lock(const fs::path& hub_dir) {
    fs::path lock = hub_dir / "lock.json";
    std::ifstream in(lock);
    if (!in) return -1;
    try {
        json j;
        in >> j;
        if (!j.contains("installed")) return 0;
        return static_cast<int>(j["installed"].size());
    } catch (...) {
        return -1;
    }
}

std::string detect_terminal_capabilities() {
    std::string caps;
    bool is_tty = HERMES_ISATTY(HERMES_FILENO(stdout)) != 0;
    if (is_tty) caps += "tty";
    const char* term = std::getenv("TERM");
    const char* colorterm = std::getenv("COLORTERM");
    if ((term && std::string(term).find("color") != std::string::npos) ||
        (term && std::string(term) != "dumb" && is_tty) || colorterm) {
        if (!caps.empty()) caps += ":";
        caps += "color";
    }
    const char* lang = std::getenv("LANG");
    if (lang && (std::string(lang).find("UTF") != std::string::npos ||
                 std::string(lang).find("utf") != std::string::npos)) {
        if (!caps.empty()) caps += ":";
        caps += "unicode";
    }
    return caps;
}

std::string gateway_lock_state(const fs::path& lock_path) {
    std::error_code ec;
    if (!fs::exists(lock_path, ec)) return "missing";
    std::ifstream in(lock_path);
    if (!in) return "unreadable";
    std::string content((std::istreambuf_iterator<char>(in)),
                         std::istreambuf_iterator<char>());
    if (content.empty()) return "unreadable";
    // Try to parse as json with "pid" key; fall back to integer.
    int pid = 0;
    try {
        auto j = json::parse(content);
        if (j.contains("pid") && j["pid"].is_number_integer()) {
            pid = j["pid"].get<int>();
        }
    } catch (...) {
        try { pid = std::stoi(content); } catch (...) { return "unreadable"; }
    }
    if (pid <= 0) return "unreadable";
#ifdef _WIN32
    // Cross-platform simplification — we can't easily check liveness
    // on Windows without additional deps; report as running.
    return "running:" + std::to_string(pid);
#else
    if (::kill(static_cast<pid_t>(pid), 0) == 0) {
        return "running:" + std::to_string(pid);
    }
    return "stale:" + std::to_string(pid);
#endif
}

// ── Check implementations ───────────────────────────────────────────────

void check_runtime_environment(Report& r, const Options& /*opts*/) {
    const char* cat = "Runtime Environment";
#if defined(__clang__)
    add_row(r, cat, Severity::Ok, "Compiler",
            "Clang " + std::string(__clang_version__));
#elif defined(__GNUC__)
    add_row(r, cat, Severity::Ok, "Compiler",
            "GCC " + std::to_string(__GNUC__) + "." +
            std::to_string(__GNUC_MINOR__));
#elif defined(_MSC_VER)
    add_row(r, cat, Severity::Ok, "Compiler",
            "MSVC " + std::to_string(_MSC_VER));
#else
    add_row(r, cat, Severity::Warn, "Compiler", "(unknown)");
#endif

#if __cplusplus >= 201703L
    add_row(r, cat, Severity::Ok, "C++ standard", "C++17+");
#else
    add_row(r, cat, Severity::Warn, "C++ standard", "(older than C++17)");
#endif

    // SQLite version.
    add_row(r, cat, Severity::Ok, "SQLite",
            std::string(sqlite3_libversion()));

    if (fts5_available()) {
        add_row(r, cat, Severity::Ok, "SQLite FTS5", "");
    } else {
        add_row(r, cat, Severity::Fail, "SQLite FTS5",
                "(FTS5 extension missing — full-text search disabled)",
                "Rebuild SQLite with SQLITE_ENABLE_FTS5");
        r.issues.push_back("Rebuild SQLite with SQLITE_ENABLE_FTS5");
    }
}

void check_config_files(Report& r, const Options& opts) {
    const char* cat = "Configuration Files";
    auto home = resolve_home(opts);

    fs::path env_path = home / ".env";
    std::error_code ec;
    if (fs::exists(env_path, ec)) {
        add_row(r, cat, Severity::Ok, ".env file exists");
        std::ifstream in(env_path);
        std::string content((std::istreambuf_iterator<char>(in)),
                             std::istreambuf_iterator<char>());
        if (has_provider_env_config(content)) {
            add_row(r, cat, Severity::Ok, "Provider API key configured");
        } else {
            add_row(r, cat, Severity::Warn, "No API key in .env",
                    "", "Run 'hermes setup' to configure API keys");
            r.issues.push_back("Run 'hermes setup' to configure API keys");
        }
    } else {
        if (opts.fix) {
            fs::create_directories(home, ec);
            std::ofstream touch(env_path);
            if (touch) {
                Row row{cat, Severity::Ok, "Created empty .env", "", "", true};
                ++r.ok_count;
                ++r.fixed_count;
                r.rows.push_back(std::move(row));
            } else {
                add_row(r, cat, Severity::Fail, ".env could not be created");
            }
        } else {
            add_row(r, cat, Severity::Fail, ".env file missing", "",
                    "Run 'hermes setup'");
            r.issues.push_back("Create ~/.hermes/.env — run 'hermes setup'");
        }
    }

    fs::path cfg_path = home / "config.yaml";
    if (fs::exists(cfg_path, ec)) {
        add_row(r, cat, Severity::Ok, "config.yaml exists");
        // Parse sanity — we use hermes::config::load_config to verify.
        try {
            auto cfg = hermes::config::load_config();
            (void)cfg;
            add_row(r, cat, Severity::Ok, "config.yaml parses");
        } catch (const std::exception& e) {
            add_row(r, cat, Severity::Fail, "config.yaml parse failed",
                    e.what(), "Backup and recreate config.yaml");
            if (opts.fix) {
                fs::path bak = cfg_path;
                bak += ".bak";
                fs::copy_file(cfg_path, bak,
                              fs::copy_options::overwrite_existing, ec);
                fs::remove(cfg_path, ec);
                ++r.fixed_count;
                add_row(r, cat, Severity::Ok,
                        "Malformed config.yaml backed up to config.yaml.bak");
            } else {
                r.issues.push_back(
                    "Malformed config.yaml — run 'hermes doctor --fix' to back up");
            }
        }
    } else {
        add_row(r, cat, Severity::Warn, "config.yaml missing",
                "(defaults will be used)");
    }
}

void check_credentials(Report& r, const Options& opts) {
    const char* cat = "Credentials";
    auto home = resolve_home(opts);

    struct Provider {
        const char* name;
        std::vector<const char*> keys;
    };
    const std::vector<Provider> providers = {
        {"OpenAI",    {"OPENAI_API_KEY"}},
        {"Anthropic", {"ANTHROPIC_API_KEY", "ANTHROPIC_TOKEN"}},
        {"Qwen",      {"DASHSCOPE_API_KEY", "QWEN_API_KEY"}},
        {"OpenRouter",{"OPENROUTER_API_KEY"}},
        {"Nous",      {"NOUS_API_KEY"}},
        {"Google",    {"GOOGLE_API_KEY", "GEMINI_API_KEY"}},
        {"Copilot",   {"GITHUB_TOKEN", "GH_TOKEN"}},
    };

    for (const auto& p : providers) {
        bool found = false;
        for (auto* k : p.keys) {
            if (!read_env_or_file(k, home).empty()) {
                found = true;
                break;
            }
        }
        if (found) {
            add_row(r, cat, Severity::Ok, p.name, "(credential present)");
        } else {
            std::string keys_joined;
            for (std::size_t i = 0; i < p.keys.size(); ++i) {
                if (i) keys_joined += " | ";
                keys_joined += p.keys[i];
            }
            add_row(r, cat, Severity::Info, p.name,
                    "(no credential; set " + keys_joined + ")");
        }
    }
}

void check_directory_structure(Report& r, const Options& opts) {
    const char* cat = "Directory Structure";
    auto home = resolve_home(opts);
    std::error_code ec;

    if (fs::exists(home, ec)) {
        add_row(r, cat, Severity::Ok, home.string() + " exists");
    } else if (opts.fix) {
        fs::create_directories(home, ec);
        if (ec) {
            add_row(r, cat, Severity::Fail, "HERMES_HOME create failed",
                    ec.message());
        } else {
            Row row{cat, Severity::Ok, "Created HERMES_HOME", "", "", true};
            ++r.ok_count; ++r.fixed_count;
            r.rows.push_back(std::move(row));
        }
    } else {
        add_row(r, cat, Severity::Warn, "HERMES_HOME missing",
                "(will be created on first use)");
    }

    for (const char* sub : {"sessions", "logs", "skills", "memories",
                             "cron", "profiles"}) {
        fs::path p = home / sub;
        if (fs::exists(p, ec)) {
            add_row(r, cat, Severity::Ok, std::string(sub) + "/ exists");
        } else if (opts.fix) {
            fs::create_directories(p, ec);
            if (!ec) {
                Row row{cat, Severity::Ok, "Created " + std::string(sub) + "/",
                        "", "", true};
                ++r.ok_count; ++r.fixed_count;
                r.rows.push_back(std::move(row));
            } else {
                add_row(r, cat, Severity::Warn, std::string(sub) + "/ not created",
                        ec.message());
            }
        } else {
            add_row(r, cat, Severity::Info, std::string(sub) + "/ missing",
                    "(created on first use)");
        }
    }
}

void check_session_db(Report& r, const Options& opts) {
    const char* cat = "Session Database";
    auto home = resolve_home(opts);
    fs::path db_path = home / "sessions.db";
    std::error_code ec;
    if (!fs::exists(db_path, ec)) {
        add_row(r, cat, Severity::Info, "sessions.db absent",
                "(created on first session)");
        return;
    }
    add_row(r, cat, Severity::Ok, "sessions.db present");
    std::string err = sqlite_integrity_check(db_path);
    if (err.empty()) {
        add_row(r, cat, Severity::Ok, "PRAGMA integrity_check = ok");
    } else {
        add_row(r, cat, Severity::Fail, "Integrity check failed", err,
                "Consider deleting sessions.db and restarting");
        r.issues.push_back("sessions.db integrity failure: " + err);
    }
    // WAL size heuristic
    fs::path wal = db_path;
    wal += "-wal";
    if (fs::exists(wal, ec)) {
        auto sz = fs::file_size(wal, ec);
        if (!ec && sz > (50ull * 1024 * 1024)) {
            add_row(r, cat, Severity::Warn, "WAL file large",
                    "(" + std::to_string(sz / (1024 * 1024)) + " MB)",
                    "Run 'hermes doctor --fix' to checkpoint");
            if (opts.fix) {
                sqlite3* db = nullptr;
                if (sqlite3_open(db_path.string().c_str(), &db) == SQLITE_OK) {
                    sqlite3_exec(db, "PRAGMA wal_checkpoint(PASSIVE);",
                                 nullptr, nullptr, nullptr);
                    sqlite3_close(db);
                    ++r.fixed_count;
                    add_row(r, cat, Severity::Ok, "WAL checkpoint performed");
                }
            } else {
                r.issues.push_back("Large WAL — run 'hermes doctor --fix'");
            }
        }
    }
}

void check_skill_index(Report& r, const Options& opts) {
    const char* cat = "Skills Hub";
    auto home = resolve_home(opts);
    fs::path hub = home / "skills" / ".hub";
    std::error_code ec;
    if (!fs::exists(hub, ec)) {
        add_row(r, cat, Severity::Info, "Skills Hub not initialised",
                "(run 'hermes skills list' to populate)");
        return;
    }
    add_row(r, cat, Severity::Ok, "Skills Hub directory present");
    int count = read_skills_hub_lock(hub);
    if (count < 0) {
        add_row(r, cat, Severity::Warn, "lock.json missing or corrupt");
    } else {
        add_row(r, cat, Severity::Ok,
                std::to_string(count) + " skill(s) installed via hub");
    }
    fs::path q = hub / "quarantine";
    int qcount = 0;
    if (fs::exists(q, ec)) {
        for (const auto& e : fs::directory_iterator(q, ec)) {
            if (e.is_directory()) ++qcount;
        }
    }
    if (qcount > 0) {
        add_row(r, cat, Severity::Warn,
                std::to_string(qcount) + " skill(s) in quarantine",
                "(pending review)");
    }
}

void check_plugin_load(Report& r, const Options& opts) {
    const char* cat = "Plugins";
    auto home = resolve_home(opts);
    fs::path plugins_dir = home / "plugins";
    std::error_code ec;
    if (!fs::exists(plugins_dir, ec)) {
        add_row(r, cat, Severity::Info, "No plugins directory",
                "(optional)");
        return;
    }
    int loadable = 0, broken = 0;
    for (const auto& entry : fs::directory_iterator(plugins_dir, ec)) {
        if (!entry.is_directory()) continue;
        fs::path manifest = entry.path() / "plugin.json";
        if (!fs::exists(manifest, ec)) { ++broken; continue; }
        try {
            std::ifstream in(manifest);
            json j;
            in >> j;
            if (j.contains("name") && j.contains("version")) ++loadable;
            else ++broken;
        } catch (...) { ++broken; }
    }
    add_row(r, cat, Severity::Ok,
            std::to_string(loadable) + " plugin(s) loadable");
    if (broken > 0) {
        add_row(r, cat, Severity::Warn,
                std::to_string(broken) + " plugin(s) broken");
        r.issues.push_back("Broken plugin manifest(s) in " + plugins_dir.string());
    }
}

void check_gateway_lock(Report& r, const Options& opts) {
    const char* cat = "Gateway";
    auto home = resolve_home(opts);
    fs::path lock = home / "gateway.lock";
    std::string state = gateway_lock_state(lock);
    if (state == "missing") {
        add_row(r, cat, Severity::Info, "Gateway not running");
    } else if (state.rfind("running:", 0) == 0) {
        add_row(r, cat, Severity::Ok, "Gateway running",
                "(pid " + state.substr(8) + ")");
    } else if (state.rfind("stale:", 0) == 0) {
        add_row(r, cat, Severity::Warn, "Stale gateway lock",
                "(pid " + state.substr(6) + ")",
                "Delete " + lock.string());
        if (opts.fix) {
            std::error_code ec;
            fs::remove(lock, ec);
            if (!ec) {
                ++r.fixed_count;
                add_row(r, cat, Severity::Ok, "Stale gateway lock removed");
            }
        } else {
            r.issues.push_back("Stale gateway lock — run 'hermes doctor --fix'");
        }
    } else {
        add_row(r, cat, Severity::Warn, "Gateway lock unreadable");
    }
}

void check_external_tools(Report& r, const Options& /*opts*/) {
    const char* cat = "External Tools";
    struct Tool { const char* name; bool required; };
    const std::array<Tool, 6> tools = {{
        {"git",     false}, {"rg",   false}, {"curl", true},
        {"docker",  false}, {"ssh",  false}, {"node", false},
    }};
    for (const auto& t : tools) {
        if (binary_on_path(t.name)) {
            add_row(r, cat, Severity::Ok, t.name);
        } else if (t.required) {
            add_row(r, cat, Severity::Fail, t.name, "(required)");
            r.issues.push_back(std::string("Install ") + t.name);
        } else {
            add_row(r, cat, Severity::Warn, t.name, "(optional)");
        }
    }
}

namespace {

struct ProbeSpec {
    const char* label;
    const char* env_key;
    const char* url;
    bool use_bearer;  // true → Authorization: Bearer <key>, false → x-api-key
};

// Runs a single HTTP probe with 10s timeout.  `detail_out` receives the
// status description.  Returns a Severity.
Severity probe_endpoint(const ProbeSpec& spec, const std::string& api_key,
                         hermes::llm::HttpTransport* transport,
                         std::string& detail_out) {
    if (!transport) {
        detail_out = "(no HTTP transport — skipped)";
        return Severity::Info;
    }
    std::unordered_map<std::string, std::string> headers;
    if (spec.use_bearer) {
        headers["Authorization"] = "Bearer " + api_key;
    } else {
        headers["x-api-key"] = api_key;
        headers["anthropic-version"] = "2023-06-01";
    }
    try {
        auto resp = transport->get(spec.url, headers);
        return classify_http_status(resp.status_code, detail_out);
    } catch (const std::exception& e) {
        detail_out = std::string("(error: ") + e.what() + ")";
        return Severity::Warn;
    }
}

}  // namespace

void check_api_reachability(Report& r, const Options& opts) {
    const char* cat = "API Reachability";
    auto home = resolve_home(opts);
    auto* transport = opts.transport ? opts.transport
                                     : hermes::llm::get_default_transport();

    const std::array<ProbeSpec, 4> probes = {{
        {"OpenRouter", "OPENROUTER_API_KEY",
         "https://openrouter.ai/api/v1/models", true},
        {"OpenAI",     "OPENAI_API_KEY",
         "https://api.openai.com/v1/models", true},
        {"Anthropic",  "ANTHROPIC_API_KEY",
         "https://api.anthropic.com/v1/models", false},
        {"DeepSeek",   "DEEPSEEK_API_KEY",
         "https://api.deepseek.com/v1/models", true},
    }};

    int configured = 0;
    for (const auto& spec : probes) {
        std::string key = read_env_or_file(spec.env_key, home);
        if (key.empty()) {
            add_row(r, cat, Severity::Info, spec.label, "(not configured)");
            continue;
        }
        ++configured;
        std::string detail;
        Severity sev = probe_endpoint(spec, key, transport, detail);
        if (sev == Severity::Ok) {
            add_row(r, cat, Severity::Ok, spec.label, "(reachable)");
        } else if (sev == Severity::Fail) {
            add_row(r, cat, Severity::Fail, spec.label, detail,
                    "Check " + std::string(spec.env_key));
            r.issues.push_back("Invalid " + std::string(spec.env_key));
        } else if (sev == Severity::Info) {
            add_row(r, cat, Severity::Info, spec.label, detail);
        } else {
            add_row(r, cat, Severity::Warn, spec.label, detail);
        }
    }
    if (configured == 0) {
        add_row(r, cat, Severity::Warn,
                "No provider credentials configured",
                "(hermes setup to configure one)");
    }
}

void check_memory_backend(Report& r, const Options& opts) {
    const char* cat = "Memory Backend";
    auto home = resolve_home(opts);
    fs::path cfg_file = home / "config.yaml";
    std::error_code ec;
    if (!fs::exists(cfg_file, ec)) {
        add_row(r, cat, Severity::Info, "Built-in memory active",
                "(no external provider configured)");
        return;
    }
    // Minimal YAML probe — we just search for "provider: honcho" / "mem0"
    std::ifstream in(cfg_file);
    std::string content((std::istreambuf_iterator<char>(in)),
                         std::istreambuf_iterator<char>());
    auto has_provider = [&](const std::string& name) {
        return content.find("provider: " + name) != std::string::npos ||
               content.find("provider: \"" + name + "\"") != std::string::npos;
    };
    if (has_provider("honcho")) {
        add_row(r, cat, Severity::Ok, "Honcho configured");
        if (!read_env_or_file("HONCHO_API_KEY", home).empty()) {
            add_row(r, cat, Severity::Ok, "HONCHO_API_KEY present");
        } else {
            add_row(r, cat, Severity::Warn, "HONCHO_API_KEY missing",
                    "", "Run 'hermes memory setup'");
            r.issues.push_back("Missing HONCHO_API_KEY");
        }
    } else if (has_provider("mem0")) {
        add_row(r, cat, Severity::Ok, "Mem0 configured");
        if (!read_env_or_file("MEM0_API_KEY", home).empty()) {
            add_row(r, cat, Severity::Ok, "MEM0_API_KEY present");
        } else {
            add_row(r, cat, Severity::Warn, "MEM0_API_KEY missing");
            r.issues.push_back("Missing MEM0_API_KEY");
        }
    } else {
        add_row(r, cat, Severity::Ok, "Built-in memory active",
                "(no external provider configured)");
    }
}

void check_mcp_servers(Report& r, const Options& opts) {
    const char* cat = "MCP Servers";
    auto home = resolve_home(opts);
    fs::path mcp_cfg = home / "mcp-servers.json";
    std::error_code ec;
    if (!fs::exists(mcp_cfg, ec)) {
        add_row(r, cat, Severity::Info, "mcp-servers.json absent");
        return;
    }
    try {
        std::ifstream in(mcp_cfg);
        json j;
        in >> j;
        int ok_count = 0, fail_count = 0;
        for (auto it = j.begin(); it != j.end(); ++it) {
            std::string cmd;
            if (it->contains("command") && (*it)["command"].is_string()) {
                cmd = (*it)["command"].get<std::string>();
            }
            if (cmd.empty()) { ++fail_count; continue; }
            auto [ok, msg] = probe_mcp_child(cmd);
            if (ok) {
                ++ok_count;
                add_row(r, cat, Severity::Ok, it.key());
            } else {
                ++fail_count;
                add_row(r, cat, Severity::Fail, it.key(), msg);
                r.issues.push_back("MCP server '" + it.key() + "' broken: " + msg);
            }
        }
        if (ok_count + fail_count == 0) {
            add_row(r, cat, Severity::Info, "No MCP servers configured");
        }
    } catch (const std::exception& e) {
        add_row(r, cat, Severity::Fail, "mcp-servers.json parse error", e.what());
        r.issues.push_back("Fix mcp-servers.json");
    }
}

void check_disk_space(Report& r, const Options& opts) {
    const char* cat = "Disk Space";
    auto home = resolve_home(opts);
    auto bytes = available_disk_bytes(home);
    if (bytes == 0) {
        add_row(r, cat, Severity::Warn, "Could not read disk space");
        return;
    }
    auto mb = bytes / (1024ull * 1024);
    std::string detail = "(" + std::to_string(mb) + " MB free)";
    if (mb < 100) {
        add_row(r, cat, Severity::Fail, "Disk nearly full", detail,
                "Free up disk space in " + home.string());
        r.issues.push_back("Disk nearly full");
    } else if (mb < 500) {
        add_row(r, cat, Severity::Warn, "Low disk space", detail);
    } else {
        add_row(r, cat, Severity::Ok, "Disk space healthy", detail);
    }
}

void check_file_permissions(Report& r, const Options& opts) {
    const char* cat = "File Permissions";
    auto home = resolve_home(opts);
    fs::path env = home / ".env";
    std::error_code ec;
    if (fs::exists(env, ec)) {
        if (is_mode_0600(env)) {
            add_row(r, cat, Severity::Ok, ".env permissions", "(0600)");
        } else {
            if (opts.fix) {
                if (chmod_0600(env)) {
                    ++r.fixed_count;
                    Row row{cat, Severity::Ok, "Fixed .env permissions",
                            "(now 0600)", "", true};
                    ++r.ok_count;
                    r.rows.push_back(std::move(row));
                } else {
                    add_row(r, cat, Severity::Fail,
                            "Could not chmod .env to 0600");
                }
            } else {
                add_row(r, cat, Severity::Warn,
                        ".env has unsafe permissions",
                        "(should be 0600)",
                        "chmod 600 " + env.string());
                r.issues.push_back(
                    "Run 'hermes doctor --fix' to chmod .env to 0600");
            }
        }
    }
    // Profile dirs — check all siblings under profiles/
    fs::path profiles = home / "profiles";
    if (fs::exists(profiles, ec)) {
        for (auto& entry : fs::directory_iterator(profiles, ec)) {
            if (!entry.is_directory()) continue;
            fs::path pe = entry.path() / ".env";
            if (fs::exists(pe, ec) && !is_mode_0600(pe)) {
                if (opts.fix && chmod_0600(pe)) {
                    ++r.fixed_count;
                    Row row{cat, Severity::Ok,
                            "Fixed " + entry.path().filename().string()
                                + "/.env permissions",
                            "(now 0600)", "", true};
                    ++r.ok_count;
                    r.rows.push_back(std::move(row));
                } else {
                    add_row(r, cat, Severity::Warn,
                            entry.path().filename().string() + "/.env",
                            "(unsafe permissions)");
                    r.issues.push_back("chmod 600 " + pe.string());
                }
            }
        }
    }
}

void check_terminal_capability(Report& r, const Options& /*opts*/) {
    const char* cat = "Terminal";
    auto caps = detect_terminal_capabilities();
    if (caps.find("tty") != std::string::npos) {
        add_row(r, cat, Severity::Ok, "TTY detected", "(" + caps + ")");
    } else {
        add_row(r, cat, Severity::Info, "Not a TTY",
                "(output piped or redirected)");
    }
}

// ── Top-level run ───────────────────────────────────────────────────────

Report run_all(const Options& opts) {
    Report r;
    check_runtime_environment(r, opts);
    check_config_files(r, opts);
    check_credentials(r, opts);
    check_directory_structure(r, opts);
    check_session_db(r, opts);
    check_skill_index(r, opts);
    check_plugin_load(r, opts);
    check_gateway_lock(r, opts);
    check_external_tools(r, opts);
    check_api_reachability(r, opts);
    check_memory_backend(r, opts);
    check_mcp_servers(r, opts);
    check_disk_space(r, opts);
    check_file_permissions(r, opts);
    check_terminal_capability(r, opts);
    return r;
}

std::string Report::to_json() const {
    json out;
    out["ok_count"] = ok_count;
    out["warn_count"] = warn_count;
    out["fail_count"] = fail_count;
    out["fixed_count"] = fixed_count;
    out["issues"] = issues;
    out["manual_issues"] = manual_issues;
    json rows_arr = json::array();
    for (const auto& row : rows) {
        rows_arr.push_back({
            {"category", row.category},
            {"severity", sev_label(row.severity)},
            {"label",    row.label},
            {"detail",   row.detail},
            {"fix_hint", row.fix_hint},
            {"auto_fixed", row.auto_fixed},
        });
    }
    out["rows"] = std::move(rows_arr);
    return out.dump(2);
}

void render(const Report& report, const Options& opts) {
    if (opts.json) {
        std::cout << report.to_json() << "\n";
        return;
    }
    Palette p = palette(opts.color);
    std::string prev_category;
    std::cout << "\n" << p.cyan << p.bold
              << "[+] Hermes Doctor" << p.reset << "\n";
    for (const auto& row : report.rows) {
        if (row.category != prev_category) {
            std::cout << "\n" << p.cyan << p.bold
                      << "# " << row.category << p.reset << "\n";
            prev_category = row.category;
        }
        const char* color_code = "";
        switch (row.severity) {
            case Severity::Ok:   color_code = p.green;  break;
            case Severity::Warn: color_code = p.yellow; break;
            case Severity::Fail: color_code = p.red;    break;
            case Severity::Info: color_code = p.dim;    break;
        }
        std::cout << "  " << color_code << symbol(row.severity) << p.reset
                  << " " << row.label;
        if (!row.detail.empty()) {
            std::cout << " " << p.dim << row.detail << p.reset;
        }
        if (row.auto_fixed) {
            std::cout << " " << p.green << "(auto-fixed)" << p.reset;
        }
        std::cout << "\n";
    }
    std::cout << "\n" << p.bold << "Summary:" << p.reset
              << " " << report.ok_count << " ok, "
              << report.warn_count << " warn, "
              << report.fail_count << " fail";
    if (report.fixed_count > 0) {
        std::cout << ", " << p.green << report.fixed_count
                  << " auto-fixed" << p.reset;
    }
    std::cout << "\n";
    if (!report.issues.empty()) {
        std::cout << "\n" << p.yellow << p.bold
                  << "Issues to address:" << p.reset << "\n";
        int i = 1;
        for (const auto& issue : report.issues) {
            std::cout << "  " << i++ << ". " << issue << "\n";
        }
        if (!opts.fix) {
            std::cout << "\n" << p.dim
                      << "Tip: run 'hermes doctor --fix' to auto-repair what's possible."
                      << p.reset << "\n";
        }
    } else if (report.fail_count == 0) {
        std::cout << p.green << "All checks passed!" << p.reset << "\n";
    }
    std::cout << "\n";
}

int run(int argc, char* argv[]) {
    Options opts;
    opts.color = HERMES_ISATTY(HERMES_FILENO(stdout)) != 0;
    for (int i = 2; i < argc; ++i) {
        std::string_view a = argv[i];
        if (a == "--fix")  opts.fix = true;
        else if (a == "--json") { opts.json = true; opts.color = false; }
        else if (a == "--no-color") opts.color = false;
        else if (a == "--help" || a == "-h") {
            std::cout << "Usage: hermes doctor [--fix] [--json] [--no-color]\n";
            return 0;
        }
    }
    auto report = run_all(opts);
    render(report, opts);
    return report.exit_code();
}

}  // namespace hermes::cli::doctor
