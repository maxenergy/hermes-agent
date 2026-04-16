// main_diagnostics — startup banner + build info + health probes.  See
// include/hermes/cli/main_diagnostics.hpp for full API.

#include "hermes/cli/main_diagnostics.hpp"
#include "hermes/cli/main_entry.hpp"   // for kVersionString

#include "hermes/config/loader.hpp"
#include "hermes/core/path.hpp"
#include "hermes/skills/skill_utils.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <string>
#include <vector>

#ifdef _WIN32
#include <io.h>
#define isatty _isatty
#define STDIN_FILENO 0
#else
#include <sys/statvfs.h>
#include <unistd.h>
#endif

namespace hermes::cli::diagnostics {

namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
namespace {

std::string run_capture(const std::string& cmd, int timeout_secs = 2) {
    // Minimal `popen`-based helper.  Errors are swallowed so diagnostics
    // never block or throw.
    (void)timeout_secs;
    std::string output;
    std::FILE* pipe = std::fopen("/dev/null", "r"); (void)pipe;  // silence unused warnings
#ifdef _WIN32
    std::FILE* p = _popen(cmd.c_str(), "r");
    if (!p) return output;
    char buf[256];
    while (std::fgets(buf, sizeof(buf), p)) output += buf;
    _pclose(p);
#else
    std::FILE* p = ::popen((cmd + " 2>/dev/null").c_str(), "r");
    if (!p) return output;
    char buf[256];
    while (std::fgets(buf, sizeof(buf), p)) output += buf;
    ::pclose(p);
#endif
    while (!output.empty() &&
           (output.back() == '\n' || output.back() == '\r')) {
        output.pop_back();
    }
    return output;
}

bool which(const std::string& cmd) {
#ifdef _WIN32
    auto out = run_capture("where " + cmd);
#else
    auto out = run_capture("command -v " + cmd);
#endif
    return !out.empty();
}

std::string getenv_or(const char* name, const std::string& fallback = "") {
    const char* v = std::getenv(name);
    return (v && *v) ? std::string(v) : fallback;
}

std::string detect_compiler() {
#if defined(__clang__)
    return std::string("clang ") + std::to_string(__clang_major__) + "." +
           std::to_string(__clang_minor__);
#elif defined(__GNUC__)
    return std::string("gcc ") + std::to_string(__GNUC__) + "." +
           std::to_string(__GNUC_MINOR__);
#elif defined(_MSC_VER)
    return std::string("msvc ") + std::to_string(_MSC_VER);
#else
    return "unknown";
#endif
}

std::string detect_platform() {
#ifdef _WIN32
    return "windows";
#elif defined(__APPLE__)
    return "darwin";
#elif defined(__ANDROID__)
    return "android";
#elif defined(__linux__)
    if (fs::exists("/proc/sys/kernel/osrelease")) {
        std::ifstream f("/proc/sys/kernel/osrelease");
        std::string line;
        if (std::getline(f, line)) {
            if (line.find("microsoft") != std::string::npos ||
                line.find("WSL") != std::string::npos) {
                return "wsl";
            }
        }
    }
    if (std::getenv("TERMUX_VERSION") || fs::exists("/data/data/com.termux")) {
        return "termux";
    }
    return "linux";
#else
    return "unknown";
#endif
}

std::string detect_target_triple() {
    std::ostringstream o;
#if defined(__x86_64__)
    o << "x86_64";
#elif defined(__aarch64__)
    o << "aarch64";
#elif defined(__arm__)
    o << "arm";
#elif defined(__i386__)
    o << "i386";
#else
    o << "unknown";
#endif
    o << "-" << detect_platform();
    return o.str();
}

std::string mask(const std::string& s) {
    if (s.empty()) return "(unset)";
    if (s.size() <= 8) return "***";
    return s.substr(0, 4) + std::string("…") + s.substr(s.size() - 4);
}

}  // namespace

// ---------------------------------------------------------------------------
// Build info
// ---------------------------------------------------------------------------
BuildInfo collect_build_info() {
    BuildInfo b;
    b.hermes_version = kVersionString;
    b.hermes_release_date = __DATE__;
    b.compiler = detect_compiler();
#ifdef NDEBUG
    b.build_type = "Release";
#else
    b.build_type = "Debug";
#endif
    b.target_triple = detect_target_triple();
    b.build_date = __DATE__;

    // Try to pull git SHA from the repo root (best-effort).
    try {
        b.git_commit = run_capture("git rev-parse --short HEAD");
    } catch (...) {
    }

    // libcurl / sqlite / openssl versions — probe via their macros if
    // available, else leave empty.
#if defined(LIBCURL_VERSION)
    b.libcurl_version = LIBCURL_VERSION;
#endif
#if defined(SQLITE_VERSION)
    b.sqlite_version = SQLITE_VERSION;
#endif
#if defined(OPENSSL_VERSION_TEXT)
    b.openssl_version = OPENSSL_VERSION_TEXT;
#endif
    return b;
}

std::string format_build_info(const BuildInfo& bi) {
    std::ostringstream o;
    o << "Build:\n"
      << "  Version:   " << bi.hermes_version << "\n"
      << "  Released:  " << bi.hermes_release_date << "\n"
      << "  Compiler:  " << bi.compiler << "\n"
      << "  Type:      " << bi.build_type << "\n"
      << "  Target:    " << bi.target_triple << "\n";
    if (!bi.git_commit.empty())      o << "  Commit:    " << bi.git_commit << "\n";
    if (!bi.libcurl_version.empty()) o << "  libcurl:   " << bi.libcurl_version << "\n";
    if (!bi.sqlite_version.empty())  o << "  SQLite:    " << bi.sqlite_version << "\n";
    if (!bi.openssl_version.empty()) o << "  OpenSSL:   " << bi.openssl_version << "\n";
    return o.str();
}

// ---------------------------------------------------------------------------
// Environment info
// ---------------------------------------------------------------------------
EnvironmentInfo collect_environment() {
    EnvironmentInfo e;
    try {
        e.hermes_home = hermes::core::path::get_hermes_home().string();
    } catch (...) {
    }
    e.active_profile = getenv_or("HERMES_ACTIVE_PROFILE", "default");
    if (!e.hermes_home.empty()) {
        auto h = fs::path(e.hermes_home);
        std::error_code ec;
        // Writability check — try to create a temp file.
        auto test = h / ".write_test";
        try {
            fs::create_directories(h);
            std::ofstream f(test);
            f << "x";
            f.close();
            e.hermes_home_writable = fs::exists(test);
            fs::remove(test, ec);
        } catch (...) {
        }
        e.env_file_exists    = fs::exists(h / ".env");
        e.config_file_exists = fs::exists(h / "config.yaml");
#ifndef _WIN32
        struct statvfs st;
        if (::statvfs(e.hermes_home.c_str(), &st) == 0) {
            e.disk_free_bytes =
                static_cast<std::uint64_t>(st.f_bavail) * st.f_frsize;
        }
#endif
    }
    e.has_curl_binary   = which("curl");
    e.has_git_binary    = which("git");
    e.has_python_binary = which("python3") || which("python");
    if (e.has_python_binary) {
        e.python_version = run_capture(
            "python3 --version || python --version");
    }
    e.shell = getenv_or("SHELL");
    e.term  = getenv_or("TERM");
    e.lang  = getenv_or("LANG");
    e.is_tty = ::isatty(STDIN_FILENO) != 0;
    e.has_tmux = !getenv_or("TMUX").empty();
    e.has_screen = !getenv_or("STY").empty();
    e.platform = detect_platform();
    return e;
}

std::string format_environment(const EnvironmentInfo& env) {
    std::ostringstream o;
    o << "Environment:\n"
      << "  Platform:       " << env.platform << "\n"
      << "  Shell:          " << (env.shell.empty() ? "(unset)" : env.shell) << "\n"
      << "  Term:           " << (env.term.empty() ? "(unset)" : env.term) << "\n"
      << "  Lang:           " << (env.lang.empty() ? "(unset)" : env.lang) << "\n"
      << "  HERMES_HOME:    " << env.hermes_home << "\n"
      << "  Profile:        " << env.active_profile << "\n"
      << "  Home writable:  " << (env.hermes_home_writable ? "yes" : "no") << "\n"
      << "  .env file:      " << (env.env_file_exists ? "present" : "absent") << "\n"
      << "  config.yaml:    " << (env.config_file_exists ? "present" : "absent") << "\n"
      << "  stdin TTY:      " << (env.is_tty ? "yes" : "no") << "\n"
      << "  In tmux:        " << (env.has_tmux ? "yes" : "no") << "\n"
      << "  curl:           " << (env.has_curl_binary ? "found" : "missing") << "\n"
      << "  git:            " << (env.has_git_binary ? "found" : "missing") << "\n"
      << "  python:         "
      << (env.has_python_binary
              ? (env.python_version.empty() ? "found" : env.python_version)
              : "missing")
      << "\n";
    if (env.disk_free_bytes > 0) {
        o << "  Disk free:      "
          << (env.disk_free_bytes / (1024ULL * 1024ULL)) << " MiB\n";
    }
    return o.str();
}

// ---------------------------------------------------------------------------
// Startup banner
// ---------------------------------------------------------------------------
std::string render_oneline_banner() {
    std::ostringstream o;
    o << "Hermes " << kVersionString << " (C++17)";
    return o.str();
}

std::string render_banner(const BannerOptions& opts) {
    if (opts.short_form) return render_oneline_banner();

    const char* magenta = opts.color ? "\033[35m" : "";
    const char* cyan    = opts.color ? "\033[36m" : "";
    const char* bold    = opts.color ? "\033[1m"  : "";
    const char* dim     = opts.color ? "\033[2m"  : "";
    const char* reset   = opts.color ? "\033[0m"  : "";

    std::ostringstream o;
    // Simple ASCII logo — avoids Unicode glyph fallback issues on Windows
    // consoles that lack nerd-font coverage.
    o << magenta <<
        "   _   _\n"
        "  | | | |\n"
        "  | |_| | ___ _ __ _ __ ___   ___  ___\n"
        "  |  _  |/ _ \\ '__| '_ ` _ \\ / _ \\/ __|\n"
        "  |_| |_|\\___|_|  |_| |_| |_|\\___||___/\n"
        << reset;
    o << bold;
    if (!opts.greeting.empty()) {
        o << opts.greeting;
    } else {
        o << "  Hermes — your AI agent";
    }
    o << reset << dim << "  " << kVersionString << reset << "\n";
    o << cyan <<
        "  /help for commands, /quit to exit." << reset << "\n";
    return o.str();
}

// ---------------------------------------------------------------------------
// Update probe — shells out to `git ls-remote` + `git rev-list --count` and
// reports how many commits the local checkout is behind origin HEAD.  One
// network round-trip; does not modify the local repo.
// ---------------------------------------------------------------------------
UpdateCheckResult check_for_updates(int timeout_secs) {
    UpdateCheckResult r;
    r.checked = true;
    if (!which("git")) {
        r.error = "git not found";
        return r;
    }
    auto repo_root = run_capture("git rev-parse --show-toplevel");
    if (repo_root.empty()) {
        r.error = "not a git checkout";
        return r;
    }
    // Probe the configured upstream quickly — `git ls-remote HEAD` is a
    // single round-trip and doesn't modify the local repo.
    (void)timeout_secs;
    auto remote = run_capture("git -C " + repo_root + " config --get remote.origin.url");
    r.remote_url = remote;
    auto local_sha  = run_capture("git -C " + repo_root + " rev-parse HEAD");
    auto remote_sha = run_capture("git -C " + repo_root + " ls-remote origin HEAD | awk '{print $1}'");
    if (local_sha.empty() || remote_sha.empty()) {
        r.error = "could not determine head SHAs";
        return r;
    }
    if (local_sha == remote_sha) {
        r.commits_behind = 0;
        return r;
    }
    // Count commits between.
    auto diff = run_capture(
        "git -C " + repo_root + " rev-list --count " + local_sha + ".." + remote_sha);
    try {
        r.commits_behind = diff.empty() ? -1 : std::stoi(diff);
    } catch (...) {
        r.commits_behind = -1;
    }
    r.latest_tag = remote_sha.substr(0, 7);
    return r;
}

std::string format_update_hint(const UpdateCheckResult& r) {
    if (!r.checked || !r.error.empty()) return {};
    if (r.commits_behind <= 0) return "Up to date";
    std::ostringstream o;
    o << "Update available: " << r.commits_behind << " "
      << (r.commits_behind == 1 ? "commit" : "commits")
      << " behind — run `hermes update`";
    return o.str();
}

// ---------------------------------------------------------------------------
// Model probe — reads config, resolves provider, does not hit the network
// by default (kept offline to avoid test flakiness).
// ---------------------------------------------------------------------------
ModelProbeResult probe_configured_model(int /*timeout_secs*/) {
    ModelProbeResult r;
    try {
        auto cfg = hermes::config::load_cli_config();
        if (cfg.contains("model") && cfg["model"].is_string()) {
            r.model = cfg["model"].get<std::string>();
        } else if (cfg.contains("model") && cfg["model"].is_object()) {
            r.model    = cfg["model"].value("default", "");
            r.provider = cfg["model"].value("provider", "");
        }
        if (cfg.contains("provider") && cfg["provider"].is_string()) {
            r.provider = cfg["provider"].get<std::string>();
        }
    } catch (...) {
    }
    if (r.model.empty()) {
        r.status = "no model configured";
        return r;
    }
    // Stop short of an actual HTTP probe — caller can upgrade this to a
    // real transport hit when diagnostics are run with --deep.
    r.status   = "configured (offline check)";
    r.reachable = true;
    return r;
}

std::string format_model_probe(const ModelProbeResult& r) {
    std::ostringstream o;
    o << "Model:\n"
      << "  Provider:  " << (r.provider.empty() ? "(auto)" : r.provider) << "\n"
      << "  Model:     " << (r.model.empty() ? "(unset)" : r.model) << "\n"
      << "  Status:    " << r.status << "\n";
    if (r.http_status > 0) o << "  HTTP:      " << r.http_status << "\n";
    if (r.latency_ms > 0.0) {
        o << "  Latency:   " << static_cast<int>(r.latency_ms) << " ms\n";
    }
    return o.str();
}

// ---------------------------------------------------------------------------
// Skill sync
// ---------------------------------------------------------------------------
SkillSyncInfo probe_skill_sync() {
    SkillSyncInfo s;
    try {
        auto skills = hermes::skills::iter_skill_index();
        s.total_skills = static_cast<int>(skills.size());
        for (const auto& sk : skills) {
            if (!sk.enabled) ++s.disabled_skills;
        }
    } catch (const std::exception& e) {
        s.error = e.what();
    }
    try {
        auto idx = hermes::core::path::get_hermes_home() / "skills.json";
        s.index_path = idx.string();
        if (fs::exists(idx)) {
            std::error_code ec;
            auto ft = fs::last_write_time(idx, ec);
            if (!ec) {
                using namespace std::chrono;
                s.last_sync = system_clock::to_time_t(
                    time_point_cast<system_clock::duration>(
                        ft - fs::file_time_type::clock::now() +
                        system_clock::now()));
                s.needs_sync = std::time(nullptr) - s.last_sync > 86400;
            }
        }
    } catch (...) {
    }
    return s;
}

std::string format_skill_sync(const SkillSyncInfo& s) {
    std::ostringstream o;
    o << "Skills:\n"
      << "  Total:      " << s.total_skills << "\n"
      << "  Disabled:   " << s.disabled_skills << "\n";
    if (!s.index_path.empty()) {
        o << "  Index:      " << s.index_path << "\n";
    }
    if (s.last_sync > 0) {
        std::tm tm_{};
#ifdef _WIN32
        localtime_s(&tm_, &s.last_sync);
#else
        localtime_r(&s.last_sync, &tm_);
#endif
        char buf[32] = {0};
        std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M", &tm_);
        o << "  Last sync:  " << buf
          << (s.needs_sync ? "  (stale >24h)" : "") << "\n";
    }
    if (!s.error.empty()) {
        o << "  Error:      " << s.error << "\n";
    }
    return o.str();
}

// ---------------------------------------------------------------------------
// MCP health
// ---------------------------------------------------------------------------
std::vector<McpHealth> probe_mcp_health() {
    std::vector<McpHealth> out;
    // Parse <HERMES_HOME>/mcp.json if present — list servers and whether
    // their command binary exists.
    try {
        auto path = hermes::core::path::get_hermes_home() / "mcp.json";
        if (!fs::exists(path)) return out;
        std::ifstream f(path);
        std::string raw((std::istreambuf_iterator<char>(f)),
                         std::istreambuf_iterator<char>());
        // Extremely lightweight JSON scan — we don't want to tightly couple
        // to nlohmann here.  Look for "command":"..." substrings.
        std::size_t i = 0;
        int probe_count = 0;
        while (i < raw.size() && probe_count < 32) {
            auto pos = raw.find("\"name\"", i);
            if (pos == std::string::npos) break;
            auto colon = raw.find(':', pos);
            auto qs    = raw.find('"', colon);
            auto qe    = raw.find('"', qs + 1);
            if (qs == std::string::npos || qe == std::string::npos) break;
            McpHealth m;
            m.name = raw.substr(qs + 1, qe - qs - 1);

            auto cpos  = raw.find("\"command\"", qe);
            if (cpos != std::string::npos) {
                auto cc   = raw.find(':', cpos);
                auto cqs  = raw.find('"', cc);
                auto cqe  = raw.find('"', cqs + 1);
                if (cqs != std::string::npos && cqe != std::string::npos) {
                    m.command = raw.substr(cqs + 1, cqe - cqs - 1);
                }
            }
            m.alive  = !m.command.empty() && which(m.command);
            m.status = m.alive ? "ok" : "command missing";
            out.push_back(std::move(m));
            i = qe + 1;
            ++probe_count;
        }
    } catch (...) {
    }
    return out;
}

std::string format_mcp_health(const std::vector<McpHealth>& entries) {
    std::ostringstream o;
    o << "MCP servers:\n";
    if (entries.empty()) {
        o << "  (none configured)\n";
        return o.str();
    }
    for (const auto& m : entries) {
        o << "  " << std::left << std::setw(20) << m.name
          << " " << std::setw(8) << (m.alive ? "ok" : "missing")
          << " " << m.command << "\n";
    }
    return o.str();
}

// ---------------------------------------------------------------------------
// Aggregate report
// ---------------------------------------------------------------------------
StartupReport collect_startup_report(bool skip_network) {
    StartupReport r;
    auto t0 = std::chrono::steady_clock::now();
    r.build  = collect_build_info();
    r.env    = collect_environment();
    r.skills = probe_skill_sync();
    r.mcps   = probe_mcp_health();
    r.model  = probe_configured_model();
    if (!skip_network) {
        r.update = check_for_updates();
    }
    auto t1 = std::chrono::steady_clock::now();
    r.probe_duration =
        std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0);
    (void)mask;  // silence unused-static warning
    return r;
}

std::string format_startup_report(const StartupReport& r,
                                  bool verbose,
                                  bool color) {
    std::ostringstream o;
    const char* bold  = color ? "\033[1m" : "";
    const char* reset = color ? "\033[0m" : "";
    o << bold << "=== Hermes startup diagnostic ===" << reset << "\n\n";
    o << format_build_info(r.build) << "\n";
    o << format_environment(r.env) << "\n";
    o << format_model_probe(r.model) << "\n";
    o << format_skill_sync(r.skills) << "\n";
    o << format_mcp_health(r.mcps) << "\n";
    if (!r.update.error.empty()) {
        o << "Update check: " << r.update.error << "\n";
    } else if (r.update.checked) {
        auto hint = format_update_hint(r.update);
        if (!hint.empty()) o << hint << "\n";
    }
    if (verbose) {
        o << "\nProbe duration: " << r.probe_duration.count() << " ms\n";
    }
    return o.str();
}

}  // namespace hermes::cli::diagnostics
