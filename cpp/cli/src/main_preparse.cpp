// main_preparse — argument pre-processing helpers (port of the Python
// helpers that run before the main subcommand dispatcher).  See
// include/hermes/cli/main_preparse.hpp for the full API.

#include "hermes/cli/main_preparse.hpp"

#include "hermes/auth/credentials.hpp"
#include "hermes/config/loader.hpp"
#include "hermes/core/path.hpp"
#include "hermes/profile/profile.hpp"
#include "hermes/state/session_db.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#ifdef _WIN32
#include <io.h>
#define isatty _isatty
#define STDIN_FILENO 0
#else
#include <unistd.h>
#endif

namespace hermes::cli::preparse {

namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------
namespace {

std::string to_lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string trim(const std::string& s) {
    std::size_t start = 0;
    std::size_t end = s.size();
    while (start < end &&
           std::isspace(static_cast<unsigned char>(s[start]))) ++start;
    while (end > start &&
           std::isspace(static_cast<unsigned char>(s[end - 1]))) --end;
    return s.substr(start, end - start);
}

std::string strip_quotes(std::string s) {
    if (s.size() >= 2 &&
        ((s.front() == '"' && s.back() == '"') ||
         (s.front() == '\'' && s.back() == '\''))) {
        s = s.substr(1, s.size() - 2);
    }
    return s;
}

// Remove argv indices [start, start+count) in place (shifts pointers left).
void argv_erase(int& argc, char** argv, int start, int count) {
    if (start < 0 || count <= 0 || start >= argc) return;
    int end = std::min(argc, start + count);
    for (int i = start; i < argc - (end - start); ++i) {
        argv[i] = argv[i + (end - start)];
    }
    argc -= (end - start);
}

// Mask a secret-like value for display.  Mirrors the Python `mask_secret`
// used by `hermes providers`.
std::string mask(const std::string& v) {
    if (v.empty()) return "(unset)";
    if (v.size() <= 8) return "***";
    return v.substr(0, 4) + std::string("…") + v.substr(v.size() - 4);
}

}  // namespace

// ---------------------------------------------------------------------------
// Profile override
// ---------------------------------------------------------------------------
std::vector<std::string> strip_profile_flag(
    const std::vector<std::string>& in,
    std::optional<std::string>* removed) {
    std::vector<std::string> out;
    out.reserve(in.size());
    bool found = false;
    for (std::size_t i = 0; i < in.size(); ++i) {
        const auto& a = in[i];
        if (!found && (a == "--profile" || a == "-p") && i + 1 < in.size()) {
            if (removed) *removed = in[i + 1];
            ++i;  // skip the value
            found = true;
            continue;
        }
        if (!found && a.rfind("--profile=", 0) == 0) {
            if (removed) *removed = a.substr(10);
            found = true;
            continue;
        }
        out.push_back(a);
    }
    return out;
}

std::string resolve_profile_home(const std::string& name) {
    if (name.empty() || name == "default") {
        return hermes::core::path::get_default_hermes_root().string();
    }
    auto dir = hermes::profile::get_profile_dir(name);
    if (!fs::exists(dir)) {
        throw std::runtime_error("Profile '" + name + "' does not exist at " +
                                 dir.string());
    }
    return dir.string();
}

ProfileOverrideResult pre_parse_profile_override(int& argc, char** argv) {
    ProfileOverrideResult out;
    if (argc < 2) return out;

    // 1. Scan for explicit --profile / -p flag.
    std::string profile_name;
    int flag_index = -1;
    int consume = 0;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if ((a == "--profile" || a == "-p") && i + 1 < argc) {
            profile_name = argv[i + 1];
            flag_index = i;
            consume = 2;
            out.source = "flag";
            break;
        }
        if (a.rfind("--profile=", 0) == 0) {
            profile_name = a.substr(10);
            flag_index = i;
            consume = 1;
            out.source = "flag";
            break;
        }
    }

    // 2. Fallback — check for an active_profile file.
    if (profile_name.empty()) {
        try {
            auto active_path =
                hermes::core::path::get_default_hermes_root() / "active_profile";
            if (fs::exists(active_path)) {
                std::ifstream f(active_path);
                std::string name;
                std::getline(f, name);
                name = trim(name);
                if (!name.empty() && name != "default") {
                    profile_name = name;
                    out.source = "active_profile";
                }
            }
        } catch (...) {
            // Ignore — corrupted file should not break startup.
        }
    }

    if (profile_name.empty()) {
        return out;
    }

    // 3. Resolve and set HERMES_HOME.
    std::string home;
    try {
        home = resolve_profile_home(profile_name);
    } catch (const std::exception& e) {
        std::cerr << "Warning: profile override failed (" << e.what()
                  << "), using default\n";
        return out;
    }

    // Set env var — later calls to get_hermes_home() pick this up.
#ifdef _WIN32
    _putenv_s("HERMES_HOME", home.c_str());
#else
    ::setenv("HERMES_HOME", home.c_str(), 1);
#endif

    out.applied = true;
    out.profile_name = profile_name;
    out.hermes_home = home;

    // 4. Strip the flag from argv.
    if (flag_index >= 0 && consume > 0) {
        argv_erase(argc, argv, flag_index, consume);
    }
    return out;
}

// ---------------------------------------------------------------------------
// Relative-time formatter
// ---------------------------------------------------------------------------
std::string format_relative_time(std::time_t ts, std::time_t now) {
    if (ts <= 0) return "?";
    if (now == 0) now = std::time(nullptr);
    double delta = std::difftime(now, ts);
    if (delta < 60.0) return "just now";
    if (delta < 3600.0) {
        int m = static_cast<int>(delta / 60.0);
        return std::to_string(m) + "m ago";
    }
    if (delta < 86400.0) {
        int h = static_cast<int>(delta / 3600.0);
        return std::to_string(h) + "h ago";
    }
    if (delta < 2 * 86400.0) return "yesterday";
    if (delta < 7 * 86400.0) {
        int d = static_cast<int>(delta / 86400.0);
        return std::to_string(d) + "d ago";
    }
    // Fall through — render ISO date.
    std::tm tm_{};
#ifdef _WIN32
    localtime_s(&tm_, &ts);
#else
    localtime_r(&ts, &tm_);
#endif
    char buf[16] = {0};
    std::strftime(buf, sizeof(buf), "%Y-%m-%d", &tm_);
    return buf;
}

// ---------------------------------------------------------------------------
// Provider probe
// ---------------------------------------------------------------------------
const std::vector<std::string>& provider_env_vars() {
    static const std::vector<std::string> vars = {
        "OPENROUTER_API_KEY",
        "OPENAI_API_KEY",
        "ANTHROPIC_API_KEY",
        "ANTHROPIC_TOKEN",
        "OPENAI_BASE_URL",
        "GOOGLE_API_KEY",
        "MISTRAL_API_KEY",
        "MINIMAX_API_KEY",
        "GROQ_API_KEY",
        "NOUS_API_KEY",
        "DASHSCOPE_API_KEY",
        "DEEPSEEK_API_KEY",
        "KIMI_API_KEY",
        "ZAI_API_KEY",
        "HUGGINGFACE_API_KEY",
        "TOGETHER_API_KEY",
        "FIREWORKS_API_KEY",
        "CEREBRAS_API_KEY",
        "PERPLEXITY_API_KEY",
        "XAI_API_KEY",
        "GITHUB_COPILOT_TOKEN",
    };
    return vars;
}

ProviderProbe probe_any_provider_configured() {
    ProviderProbe p;

    // 1. Environment variables — the cheapest probe.
    for (const auto& v : provider_env_vars()) {
        const char* val = std::getenv(v.c_str());
        if (val && *val) {
            p.configured = true;
            p.source = "env";
            p.details = v + "=" + mask(val);
            return p;
        }
    }

    // 2. ~/.hermes/.env — user-managed file.
    try {
        auto env_path = hermes::core::path::get_hermes_home() / ".env";
        if (fs::exists(env_path)) {
            std::ifstream f(env_path);
            std::string line;
            while (std::getline(f, line)) {
                auto trimmed = trim(line);
                if (trimmed.empty() || trimmed[0] == '#') continue;
                auto eq = trimmed.find('=');
                if (eq == std::string::npos) continue;
                std::string key = trim(trimmed.substr(0, eq));
                std::string val = trim(trimmed.substr(eq + 1));
                val = strip_quotes(val);
                if (val.empty()) continue;
                for (const auto& var : provider_env_vars()) {
                    if (key == var) {
                        p.configured = true;
                        p.source = "env_file";
                        p.details = key;
                        return p;
                    }
                }
            }
        }
    } catch (...) {
    }

    // 3. auth.json — Nous/OAuth credentials.
    try {
        auto auth_path = hermes::core::path::get_hermes_home() / "auth.json";
        if (fs::exists(auth_path)) {
            std::ifstream f(auth_path);
            nlohmann::json j;
            f >> j;
            if (j.contains("active_provider") &&
                j["active_provider"].is_string() &&
                !j["active_provider"].get<std::string>().empty()) {
                p.configured = true;
                p.source = "auth_json";
                p.details = j["active_provider"].get<std::string>();
                return p;
            }
        }
    } catch (...) {
    }

    // 4. config.yaml — model-as-dict with provider/base_url/api_key set.
    try {
        auto cfg = hermes::config::load_cli_config();
        if (cfg.contains("model") && cfg["model"].is_object()) {
            const auto& m = cfg["model"];
            auto take_str = [&](const char* k) -> std::string {
                if (!m.contains(k) || !m[k].is_string()) return {};
                return trim(m[k].get<std::string>());
            };
            if (!take_str("provider").empty() ||
                !take_str("base_url").empty() ||
                !take_str("api_key").empty()) {
                p.configured = true;
                p.source = "config";
                p.details = take_str("provider");
                return p;
            }
        }
    } catch (...) {
    }

    return p;
}

bool has_any_provider_configured() {
    return probe_any_provider_configured().configured;
}

// ---------------------------------------------------------------------------
// Session lookup — primary path queries SessionDB (SQLite at
// <HERMES_HOME>/sessions.db), matching Python's resolvers.  Falls back to
// scanning <HERMES_HOME>/sessions/*.json so legacy file-based sessions
// still resolve.
// ---------------------------------------------------------------------------
namespace {

struct SessionSummary {
    std::string id;
    std::string title;
    std::string preview;
    std::string source;
    std::time_t last_active = 0;
};

std::vector<SessionSummary> load_session_summaries(std::size_t max_rows = 512) {
    std::vector<SessionSummary> out;
    try {
        auto dir = hermes::core::path::get_hermes_home() / "sessions";
        if (!fs::is_directory(dir)) return out;
        std::vector<fs::directory_entry> entries;
        for (const auto& e : fs::directory_iterator(dir)) {
            if (!e.is_regular_file()) continue;
            if (e.path().extension() != ".json") continue;
            entries.push_back(e);
        }
        std::sort(entries.begin(), entries.end(),
                  [](const auto& a, const auto& b) {
                      std::error_code ec;
                      auto ta = fs::last_write_time(a.path(), ec);
                      auto tb = fs::last_write_time(b.path(), ec);
                      return ta > tb;
                  });
        std::size_t taken = 0;
        for (const auto& e : entries) {
            if (taken >= max_rows) break;
            try {
                std::ifstream f(e.path());
                nlohmann::json j;
                f >> j;
                SessionSummary s;
                s.id = j.value("id", e.path().stem().string());
                s.title = j.value("title", "");
                s.preview = j.value("preview", "");
                s.source = j.value("source", "cli");
                if (j.contains("last_active") && j["last_active"].is_number()) {
                    s.last_active =
                        static_cast<std::time_t>(j["last_active"].get<double>());
                } else {
                    std::error_code ec;
                    auto ft = fs::last_write_time(e.path(), ec);
                    if (!ec) {
                        using namespace std::chrono;
                        s.last_active = system_clock::to_time_t(
                            time_point_cast<system_clock::duration>(
                                ft - fs::file_time_type::clock::now() +
                                system_clock::now()));
                    }
                }
                out.push_back(std::move(s));
                ++taken;
            } catch (...) {
                // skip malformed
            }
        }
    } catch (...) {
    }
    return out;
}

}  // namespace

std::optional<std::string> resolve_last_cli_session() {
    // Primary: SessionDB query for the most recent cli-sourced session.
    try {
        hermes::state::SessionDB db;
        auto rows = db.list_sessions(64, 0);
        for (const auto& r : rows) {
            if (r.source.empty() || r.source == "cli") return r.id;
        }
    } catch (const std::exception&) {
        // fall through to JSON scan
    }
    auto summaries = load_session_summaries(32);
    for (const auto& s : summaries) {
        if (s.source.empty() || s.source == "cli") return s.id;
    }
    return std::nullopt;
}

std::optional<std::string> resolve_session_by_name_or_id(
    const std::string& name_or_id) {
    if (name_or_id.empty()) return std::nullopt;
    // Primary: SessionDB — exact id, then title exact/substring, then id prefix.
    try {
        hermes::state::SessionDB db;
        if (auto direct = db.get_session(name_or_id)) {
            return direct->id;
        }
        auto rows = db.list_sessions(512, 0);
        auto lower = to_lower(name_or_id);
        for (const auto& r : rows) {
            if (r.title && to_lower(*r.title) == lower) return r.id;
        }
        for (const auto& r : rows) {
            if (r.title &&
                to_lower(*r.title).find(lower) != std::string::npos) {
                return r.id;
            }
        }
        for (const auto& r : rows) {
            if (r.id.rfind(name_or_id, 0) == 0) return r.id;
        }
    } catch (const std::exception&) {
        // fall through to JSON scan
    }
    auto summaries = load_session_summaries(512);
    auto lower = to_lower(name_or_id);
    // 1. Exact id match wins.
    for (const auto& s : summaries) {
        if (s.id == name_or_id) return s.id;
    }
    // 2. Case-insensitive title exact match.
    for (const auto& s : summaries) {
        if (!s.title.empty() && to_lower(s.title) == lower) return s.id;
    }
    // 3. Substring in title or preview.
    for (const auto& s : summaries) {
        if (!s.title.empty() && to_lower(s.title).find(lower) != std::string::npos) {
            return s.id;
        }
    }
    for (const auto& s : summaries) {
        if (!s.preview.empty() &&
            to_lower(s.preview).find(lower) != std::string::npos) {
            return s.id;
        }
    }
    // 4. Id prefix (short sha style).
    for (const auto& s : summaries) {
        if (s.id.rfind(name_or_id, 0) == 0) return s.id;
    }
    return std::nullopt;
}

// ---------------------------------------------------------------------------
// TTY guard
// ---------------------------------------------------------------------------
bool require_tty(const std::string& command_name, std::ostream* err) {
    bool is_tty = ::isatty(STDIN_FILENO) != 0;
    if (is_tty) return true;
    auto& stream = err ? *err : std::cerr;
    stream << "Error: 'hermes " << command_name
           << "' requires an interactive terminal.\n"
           << "It cannot be run through a pipe or non-interactive subprocess.\n"
           << "Run it directly in your terminal instead.\n";
    return false;
}

int require_tty_or_exit_code(const std::string& command_name) {
    return require_tty(command_name) ? 0 : 1;
}

// ---------------------------------------------------------------------------
// --continue / -c trailing-word coalescing
// ---------------------------------------------------------------------------
std::vector<std::string> coalesce_session_name_args(
    const std::vector<std::string>& argv) {
    std::vector<std::string> out;
    out.reserve(argv.size());
    for (std::size_t i = 0; i < argv.size(); ++i) {
        const auto& a = argv[i];
        if ((a == "-c" || a == "--continue") && i + 1 < argv.size()) {
            // Consume trailing bare words (not starting with `-`) and merge.
            out.push_back(a);
            std::string merged;
            std::size_t j = i + 1;
            while (j < argv.size() && !argv[j].empty() && argv[j][0] != '-') {
                if (!merged.empty()) merged.push_back(' ');
                merged += argv[j];
                ++j;
            }
            if (!merged.empty()) {
                out.push_back(merged);
            }
            i = j - 1;
        } else {
            out.push_back(a);
        }
    }
    return out;
}

// ---------------------------------------------------------------------------
// CLI mode detection
// ---------------------------------------------------------------------------
std::string detect_cli_mode(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--input" || a == "-i") return "file";
        if (a.rfind("--input=", 0) == 0) return "file";
    }
    if (!::isatty(STDIN_FILENO)) return "pipe";
    return "cli";
}

std::string extract_input_file(int& argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if ((a == "--input" || a == "-i") && i + 1 < argc) {
            std::string path = argv[i + 1];
            argv_erase(argc, argv, i, 2);
            return path;
        }
        if (a.rfind("--input=", 0) == 0) {
            std::string path = a.substr(8);
            argv_erase(argc, argv, i, 1);
            return path;
        }
    }
    return {};
}

// ---------------------------------------------------------------------------
// Global flag pre-parse
// ---------------------------------------------------------------------------
namespace {

// Consume a single flag (and its value for value-taking flags).  Returns
// the captured value (or empty string) and mutates argv to erase the
// consumed slots.  Accepts `--flag`, `--flag=VALUE`, `--flag VALUE`,
// and (when short_name is non-null) `-X` / `-X VALUE` / `-XVALUE`.
std::string consume_flag(int& argc, char** argv,
                         const char* long_name,
                         const char* short_name,
                         bool value_taking) {
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == long_name || (short_name && a == short_name)) {
            if (!value_taking) {
                argv_erase(argc, argv, i, 1);
                return "1";
            }
            if (i + 1 >= argc) {
                argv_erase(argc, argv, i, 1);
                return {};
            }
            std::string v = argv[i + 1];
            argv_erase(argc, argv, i, 2);
            return v;
        }
        std::string prefix = std::string(long_name) + "=";
        if (a.rfind(prefix, 0) == 0) {
            std::string v = a.substr(prefix.size());
            argv_erase(argc, argv, i, 1);
            return v.empty() ? std::string("1") : v;
        }
    }
    return {};
}

}  // namespace

GlobalFlags pre_parse_global_flags(int& argc, char** argv) {
    GlobalFlags out;
    out.yolo = !consume_flag(argc, argv, "--yolo", nullptr, false).empty();
    out.worktree =
        !consume_flag(argc, argv, "--worktree", "-w", false).empty();
    out.pass_session_id =
        !consume_flag(argc, argv, "--pass-session-id", nullptr, false).empty();
    out.verbose = !consume_flag(argc, argv, "--verbose", "-v", false).empty();
    out.quiet = !consume_flag(argc, argv, "--quiet", "-Q", false).empty();
    out.source = consume_flag(argc, argv, "--source", nullptr, true);

    out.resume = consume_flag(argc, argv, "--resume", "-r", true);

    // --continue / -c is tricky because value is optional.  Peek at the next
    // token to decide — if it starts with `-`, treat as bare flag.
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--continue" || a == "-c") {
            out.continue_flag = true;
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                out.continue_name = argv[i + 1];
                argv_erase(argc, argv, i, 2);
            } else {
                argv_erase(argc, argv, i, 1);
            }
            break;
        }
        if (a.rfind("--continue=", 0) == 0) {
            out.continue_flag = true;
            out.continue_name = a.substr(11);
            argv_erase(argc, argv, i, 1);
            break;
        }
    }

    // --max-turns INT
    std::string mt = consume_flag(argc, argv, "--max-turns", nullptr, true);
    if (!mt.empty()) {
        try {
            out.max_turns = std::stoi(mt);
        } catch (...) {
            out.max_turns = 0;
        }
    }

    // --skills / -s — repeatable and/or comma-separated.
    while (true) {
        std::string v = consume_flag(argc, argv, "--skills", "-s", true);
        if (v.empty()) break;
        std::size_t start = 0;
        for (std::size_t i = 0; i <= v.size(); ++i) {
            if (i == v.size() || v[i] == ',') {
                auto piece = trim(v.substr(start, i - start));
                if (!piece.empty()) out.skills.push_back(piece);
                start = i + 1;
            }
        }
    }

    return out;
}

}  // namespace hermes::cli::preparse
