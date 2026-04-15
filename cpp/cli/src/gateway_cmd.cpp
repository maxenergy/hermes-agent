// C++17 port of hermes_cli/gateway.py — full operator-facing surface.
//
// This file is intentionally fat: it mirrors the Python module's
// service-lifecycle, log-streaming, doctor, and profile-awareness
// responsibilities.  No unit depends on a live systemd — the
// renderers and discovery helpers degrade gracefully when the
// required tools aren't on the host.
#include "hermes/cli/gateway_cmd.hpp"

#include "hermes/cli/colors.hpp"
#include "hermes/core/path.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <regex>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#ifndef _WIN32
#  include <cerrno>
#  include <csignal>
#  include <dirent.h>
#  include <sys/stat.h>
#  include <sys/types.h>
#  include <unistd.h>
#endif

namespace hermes::cli::gateway_cmd {

namespace fs = std::filesystem;
namespace col = hermes::cli::colors;

// ===========================================================================
// Small utilities
// ===========================================================================

namespace {

fs::path home_dir() {
    const char* h = std::getenv("HOME");
    return h ? fs::path(h) : fs::path("/tmp");
}

fs::path user_systemd_dir() {
    return home_dir() / ".config" / "systemd" / "user";
}

fs::path launchd_dir() {
    return home_dir() / "Library" / "LaunchAgents";
}

fs::path log_file_path() {
    return hermes::core::path::get_hermes_home() / "logs" / "gateway.log";
}

[[maybe_unused]] fs::path pid_file_path() {
    return hermes::core::path::get_hermes_home() / "gateway.pid";
}

fs::path config_file_path() {
    return hermes::core::path::get_hermes_home() / "config.yaml";
}

struct CmdOut {
    int code = -1;
    std::string out;
};

CmdOut run_cmd(const std::string& cmd) {
    CmdOut r;
#ifndef _WIN32
    std::string full = cmd + " 2>/dev/null";
    FILE* p = popen(full.c_str(), "r");
    if (!p) return r;
    char buf[4096];
    while (fgets(buf, sizeof(buf), p)) r.out += buf;
    int status = pclose(p);
    r.code = status;
#else
    (void)cmd;
#endif
    return r;
}

std::string rtrim(std::string s) {
    while (!s.empty() &&
           (s.back() == '\n' || s.back() == '\r' ||
            s.back() == ' '  || s.back() == '\t')) {
        s.pop_back();
    }
    return s;
}

std::string to_upper(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

bool which(const std::string& tool) {
#ifdef _WIN32
    (void)tool;
    return false;
#else
    auto r = run_cmd("command -v " + tool);
    return !rtrim(r.out).empty();
#endif
}

[[maybe_unused]] bool is_pid_alive(int pid) {
#ifdef _WIN32
    (void)pid; return false;
#else
    if (pid <= 0) return false;
    if (::kill(pid, 0) == 0) return true;
    return errno != ESRCH;
#endif
}

std::string read_proc_cmdline(int pid) {
    std::string path = "/proc/" + std::to_string(pid) + "/cmdline";
    std::ifstream f(path, std::ios::binary);
    if (!f) return "";
    std::string s((std::istreambuf_iterator<char>(f)),
                  std::istreambuf_iterator<char>());
    std::replace(s.begin(), s.end(), '\0', ' ');
    return rtrim(s);
}

// Parse /proc/<pid>/environ for KEY=... value; empty if unreadable.
std::string read_proc_env(int pid, const std::string& key) {
    std::string path = "/proc/" + std::to_string(pid) + "/environ";
    std::ifstream f(path, std::ios::binary);
    if (!f) return "";
    std::string s((std::istreambuf_iterator<char>(f)),
                  std::istreambuf_iterator<char>());
    std::string needle = key + "=";
    std::size_t pos = 0;
    while (pos < s.size()) {
        std::size_t end = s.find('\0', pos);
        if (end == std::string::npos) end = s.size();
        std::string_view entry(s.data() + pos, end - pos);
        if (entry.substr(0, needle.size()) == needle) {
            return std::string(entry.substr(needle.size()));
        }
        pos = end + 1;
    }
    return "";
}

// Read `Name:\t<value>` style field from /proc/<pid>/status.
std::uint64_t read_proc_status_kb(int pid, const std::string& key) {
    std::string path = "/proc/" + std::to_string(pid) + "/status";
    std::ifstream f(path);
    if (!f) return 0;
    std::string line;
    while (std::getline(f, line)) {
        if (line.rfind(key + ":", 0) == 0) {
            std::istringstream is(line.substr(key.size() + 1));
            std::uint64_t v = 0;
            is >> v;
            return v;
        }
    }
    return 0;
}

// Seconds the process has been running, from /proc/<pid>/stat start_time.
// Returns 0 on any parse failure.
std::uint64_t read_proc_uptime_sec(int pid) {
#ifdef _WIN32
    (void)pid; return 0;
#else
    std::ifstream stat_f("/proc/" + std::to_string(pid) + "/stat");
    if (!stat_f) return 0;
    std::string content((std::istreambuf_iterator<char>(stat_f)),
                        std::istreambuf_iterator<char>());
    // Skip to 22nd field (start_time, clock ticks since boot).
    auto rparen = content.rfind(')');
    if (rparen == std::string::npos) return 0;
    std::istringstream is(content.substr(rparen + 1));
    std::string tok;
    for (int i = 0; i < 20; ++i) is >> tok;  // state + 19 more fields
    std::uint64_t start_ticks = 0;
    is >> start_ticks;
    if (start_ticks == 0) return 0;
    long hz = sysconf(_SC_CLK_TCK);
    if (hz <= 0) hz = 100;

    std::ifstream up("/proc/uptime");
    if (!up) return 0;
    double boot_uptime = 0.0;
    up >> boot_uptime;
    double start_sec = static_cast<double>(start_ticks) / static_cast<double>(hz);
    double proc_uptime = boot_uptime - start_sec;
    if (proc_uptime < 0) return 0;
    return static_cast<std::uint64_t>(proc_uptime);
#endif
}

[[maybe_unused]] std::string env_or_empty(const char* name) {
    const char* v = std::getenv(name);
    return v ? std::string(v) : "";
}

// Attempt to atomically replace `target` with `content`.
bool atomic_write(const fs::path& target, const std::string& content) {
    std::error_code ec;
    fs::create_directories(target.parent_path(), ec);
    auto tmp = target.parent_path() / (target.filename().string() + ".tmp");
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) return false;
        f << content;
        if (!f) return false;
    }
    fs::rename(tmp, target, ec);
    if (ec) {
        fs::remove(tmp, ec);
        return false;
    }
    return true;
}

// ===========================================================================
// Log filter helpers
// ===========================================================================

constexpr const char* kLevelNames[] = {
    "TRACE", "DEBUG", "INFO", "WARN", "WARNING", "ERROR", "CRITICAL", "FATAL",
};

}  // namespace

int level_rank(const std::string& level) {
    std::string up = to_upper(level);
    if (up == "TRACE")    return 10;
    if (up == "DEBUG")    return 20;
    if (up == "INFO")     return 30;
    if (up == "WARN")     return 40;
    if (up == "WARNING")  return 40;
    if (up == "ERROR")    return 50;
    if (up == "CRITICAL") return 60;
    if (up == "FATAL")    return 60;
    return 0;
}

std::string parse_log_level(const std::string& line) {
    // Common formats:
    //   2024-01-02 12:34:56 INFO message...
    //   [INFO] message
    //   INFO: message
    for (const char* name : kLevelNames) {
        std::string bracket = "[" + std::string(name) + "]";
        if (line.find(bracket) != std::string::npos) return name;
        std::string padded = " " + std::string(name) + " ";
        if (line.find(padded) != std::string::npos) return name;
        std::string colon = std::string(name) + ":";
        if (line.rfind(colon, 0) == 0) return name;
    }
    return "";
}

bool log_line_matches(const std::string& line, const LogFilter& filter) {
    bool match = true;
    if (filter.grep.has_value()) {
        match = line.find(*filter.grep) != std::string::npos;
    }
    if (match && filter.regex.has_value()) {
        try {
            std::regex re(*filter.regex);
            match = std::regex_search(line, re);
        } catch (const std::regex_error&) {
            match = false;
        }
    }
    if (match && filter.min_level.has_value()) {
        int want = level_rank(*filter.min_level);
        int have = level_rank(parse_log_level(line));
        if (have < want) match = false;
    }
    if (match && filter.since.has_value()) {
        // Naive lexicographic prefix compare — works for ISO timestamps.
        // We accept `line >= since`.
        const auto& s = *filter.since;
        if (line.size() < s.size() ||
            line.compare(0, s.size(), s) < 0) {
            match = false;
        }
    }
    return filter.invert ? !match : match;
}

std::vector<std::string> filter_log_lines(
    const std::vector<std::string>& lines,
    const LogFilter& filter,
    std::size_t max_lines) {
    std::vector<std::string> out;
    for (const auto& l : lines) {
        if (log_line_matches(l, filter)) {
            out.push_back(l);
            if (max_lines && out.size() >= max_lines) break;
        }
    }
    return out;
}

LogsOptions parse_logs_args(const std::vector<std::string>& argv) {
    LogsOptions opts;
    for (std::size_t i = 0; i < argv.size(); ++i) {
        const auto& a = argv[i];
        if (a == "--follow" || a == "-f") {
            opts.follow = true;
        } else if ((a == "--tail" || a == "-n") && i + 1 < argv.size()) {
            try { opts.tail = std::stoi(argv[++i]); } catch (...) {}
        } else if ((a == "--filter" || a == "--regex") && i + 1 < argv.size()) {
            opts.filter.regex = argv[++i];
        } else if ((a == "--grep") && i + 1 < argv.size()) {
            opts.filter.grep = argv[++i];
        } else if ((a == "--level") && i + 1 < argv.size()) {
            opts.filter.min_level = argv[++i];
        } else if ((a == "--since") && i + 1 < argv.size()) {
            opts.filter.since = argv[++i];
        } else if (a == "--invert" || a == "-v") {
            opts.filter.invert = true;
        } else if (a == "--journald" || a == "--journal") {
            opts.journald = true;
        }
    }
    return opts;
}

// ===========================================================================
// Service unit rendering
// ===========================================================================

std::string systemd_service_name(const std::string& profile) {
    if (profile.empty()) return "hermes-gateway.service";
    return "hermes-gateway@" + profile + ".service";
}

std::string launchd_label(const std::string& profile) {
    if (profile.empty()) return "com.hermes.gateway";
    return "com.hermes.gateway." + profile;
}

std::string render_service_unit(const std::string& exec_path) {
    std::ostringstream os;
    os << "[Unit]\n"
       << "Description=Hermes messaging gateway\n"
       << "After=network-online.target\n"
       << "Wants=network-online.target\n"
       << "\n[Service]\n"
       << "Type=simple\n"
       << "ExecStart=" << exec_path << " gateway --run\n"
       << "ExecReload=/bin/kill -HUP $MAINPID\n"
       << "Restart=on-failure\n"
       << "RestartSec=5\n"
       << "StandardOutput=journal\n"
       << "StandardError=journal\n"
       << "\n[Install]\n"
       << "WantedBy=default.target\n";
    return os.str();
}

std::string render_service_unit_for_profile(const std::string& exec_path,
                                            const std::string& profile) {
    std::ostringstream os;
    os << "[Unit]\n"
       << "Description=Hermes messaging gateway";
    if (!profile.empty()) os << " (profile=" << profile << ")";
    os << "\nAfter=network-online.target\n"
       << "Wants=network-online.target\n"
       << "\n[Service]\n"
       << "Type=simple\n"
       << "Environment=HERMES_PROFILE=" << profile << "\n"
       << "ExecStart=" << exec_path << " gateway --run\n"
       << "ExecReload=/bin/kill -HUP $MAINPID\n"
       << "Restart=on-failure\n"
       << "RestartSec=5\n"
       << "SyslogIdentifier=hermes-gateway-" << (profile.empty() ? "default" : profile) << "\n"
       << "\n[Install]\n"
       << "WantedBy=default.target\n";
    return os.str();
}

std::string render_launchd_plist(const std::string& label,
                                 const std::string& exec_path) {
    std::ostringstream os;
    os << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
       << "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" "
       << "\"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
       << "<plist version=\"1.0\">\n"
       << "<dict>\n"
       << "  <key>Label</key>\n  <string>" << label << "</string>\n"
       << "  <key>ProgramArguments</key>\n"
       << "  <array>\n"
       << "    <string>" << exec_path << "</string>\n"
       << "    <string>gateway</string>\n"
       << "    <string>--run</string>\n"
       << "  </array>\n"
       << "  <key>RunAtLoad</key>\n  <true/>\n"
       << "  <key>KeepAlive</key>\n  <true/>\n"
       << "  <key>StandardOutPath</key>\n"
       << "  <string>" << log_file_path().string() << "</string>\n"
       << "  <key>StandardErrorPath</key>\n"
       << "  <string>" << log_file_path().string() << "</string>\n"
       << "</dict>\n"
       << "</plist>\n";
    return os.str();
}

std::string render_launchd_plist_for_profile(const std::string& exec_path,
                                             const std::string& profile) {
    std::string label = launchd_label(profile);
    std::ostringstream os;
    os << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
       << "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" "
       << "\"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
       << "<plist version=\"1.0\">\n"
       << "<dict>\n"
       << "  <key>Label</key>\n  <string>" << label << "</string>\n"
       << "  <key>ProgramArguments</key>\n"
       << "  <array>\n"
       << "    <string>" << exec_path << "</string>\n"
       << "    <string>gateway</string>\n"
       << "    <string>--run</string>\n"
       << "  </array>\n"
       << "  <key>EnvironmentVariables</key>\n"
       << "  <dict>\n"
       << "    <key>HERMES_PROFILE</key>\n    <string>" << profile << "</string>\n"
       << "  </dict>\n"
       << "  <key>RunAtLoad</key>\n  <true/>\n"
       << "  <key>KeepAlive</key>\n  <true/>\n"
       << "</dict>\n"
       << "</plist>\n";
    return os.str();
}

// ===========================================================================
// Process discovery
// ===========================================================================

std::vector<int> find_gateway_pids() {
    std::vector<int> out;
#ifndef _WIN32
    DIR* d = opendir("/proc");
    if (!d) return out;
    struct dirent* ent;
    while ((ent = readdir(d))) {
        if (ent->d_type != DT_DIR) continue;
        std::string name = ent->d_name;
        if (name.empty() || !std::all_of(name.begin(), name.end(),
                                          [](char c) { return std::isdigit(static_cast<unsigned char>(c)); })) {
            continue;
        }
        std::string cmdline = read_proc_cmdline(std::stoi(name));
        if (cmdline.empty()) continue;
        if (cmdline.find("hermes gateway") != std::string::npos ||
            cmdline.find("gateway/run") != std::string::npos ||
            cmdline.find("hermes_cpp gateway") != std::string::npos ||
            cmdline.find("hermes-gateway") != std::string::npos) {
            int pid = 0;
            try { pid = std::stoi(name); } catch (...) { pid = 0; }
            if (pid > 0) out.push_back(pid);
        }
    }
    closedir(d);
#endif
    std::sort(out.begin(), out.end());
    return out;
}

GatewayProcess describe_process(int pid) {
    GatewayProcess p;
    p.pid = pid;
#ifndef _WIN32
    p.cmdline = read_proc_cmdline(pid);
    p.profile = read_proc_env(pid, "HERMES_PROFILE");
    p.hermes_home = read_proc_env(pid, "HERMES_HOME");
    p.rss_kb = read_proc_status_kb(pid, "VmRSS");
    p.uptime_sec = read_proc_uptime_sec(pid);
#endif
    return p;
}

std::vector<GatewayProcess> find_gateway_processes() {
    std::vector<GatewayProcess> out;
    for (int pid : find_gateway_pids()) {
        out.push_back(describe_process(pid));
    }
    return out;
}

std::vector<GatewayProcess> filter_by_profile(
    const std::vector<GatewayProcess>& all,
    const std::string& profile) {
    std::vector<GatewayProcess> out;
    for (const auto& p : all) {
        if (p.profile == profile) out.push_back(p);
    }
    return out;
}

// ===========================================================================
// systemd / launchd introspection
// ===========================================================================

bool has_systemctl()   { return which("systemctl"); }
bool has_launchctl()   { return which("launchctl"); }
bool has_journalctl()  { return which("journalctl"); }

static fs::path service_unit_path_for(const std::string& profile) {
    return user_systemd_dir() / systemd_service_name(profile);
}

static fs::path launchd_plist_path_for(const std::string& profile) {
    return launchd_dir() / (launchd_label(profile) + ".plist");
}

bool systemd_service_installed(const std::string& profile) {
    return fs::exists(service_unit_path_for(profile));
}

bool systemd_service_active(const std::string& profile) {
    if (!has_systemctl()) return false;
    auto r = run_cmd("systemctl --user is-active " + systemd_service_name(profile));
    return rtrim(r.out) == "active";
}

bool launchd_service_installed(const std::string& profile) {
    return fs::exists(launchd_plist_path_for(profile));
}

bool launchd_service_loaded(const std::string& profile) {
    if (!has_launchctl()) return false;
    auto r = run_cmd("launchctl list " + launchd_label(profile));
    return r.code == 0 && !r.out.empty();
}

// ===========================================================================
// Doctor
// ===========================================================================

std::vector<std::pair<std::string, std::string>> gateway_credential_env_vars() {
    return {
        {"Telegram",   "TELEGRAM_BOT_TOKEN"},
        {"Discord",    "DISCORD_BOT_TOKEN"},
        {"Slack",      "SLACK_BOT_TOKEN"},
        {"WhatsApp",   "WHATSAPP_ACCESS_TOKEN"},
        {"Signal",     "SIGNAL_CLI_PATH"},
        {"Matrix",     "MATRIX_ACCESS_TOKEN"},
        {"BlueBubbles","BLUEBUBBLES_SERVER_URL"},
        {"Mattermost", "MATTERMOST_TOKEN"},
        {"DingTalk",   "DINGTALK_APP_KEY"},
        {"Feishu",     "FEISHU_APP_ID"},
        {"WeCom",      "WECOM_CORP_ID"},
        {"Email",      "HERMES_EMAIL_IMAP_USER"},
    };
}

std::vector<DoctorResult> run_doctor_checks() {
    std::vector<DoctorResult> r;

    auto home = hermes::core::path::get_hermes_home();
    {
        DoctorResult dr;
        dr.name = "hermes home directory";
        std::error_code ec;
        if (fs::exists(home, ec)) {
            dr.status = CheckStatus::OK;
            dr.detail = home.string();
        } else {
            dr.status = CheckStatus::WARN;
            dr.detail = "missing: " + home.string();
        }
        r.push_back(dr);
    }
    {
        DoctorResult dr;
        dr.name = "config.yaml";
        auto cfg = config_file_path();
        std::error_code ec;
        if (fs::exists(cfg, ec)) {
            dr.status = CheckStatus::OK;
            dr.detail = cfg.string();
        } else {
            dr.status = CheckStatus::WARN;
            dr.detail = "not found — run `hermes setup`";
        }
        r.push_back(dr);
    }
    {
        DoctorResult dr;
        dr.name = "log directory writable";
        auto dir = home / "logs";
        std::error_code ec;
        fs::create_directories(dir, ec);
        if (ec) {
            dr.status = CheckStatus::FAIL;
            dr.detail = ec.message();
        } else {
            // Try a probe file.
            auto probe = dir / ".doctor.probe";
            std::ofstream f(probe);
            if (f) {
                f << "ok";
                f.close();
                fs::remove(probe, ec);
                dr.status = CheckStatus::OK;
                dr.detail = dir.string();
            } else {
                dr.status = CheckStatus::FAIL;
                dr.detail = "cannot create files in " + dir.string();
            }
        }
        r.push_back(dr);
    }
    {
        DoctorResult dr;
        dr.name = "systemctl (Linux service manager)";
        if (has_systemctl()) {
            dr.status = CheckStatus::OK;
            dr.detail = "available";
        } else {
#ifdef __linux__
            dr.status = CheckStatus::WARN;
            dr.detail = "not found on PATH — service install/uninstall disabled";
#else
            dr.status = CheckStatus::SKIP;
            dr.detail = "not applicable on this platform";
#endif
        }
        r.push_back(dr);
    }
    {
        DoctorResult dr;
        dr.name = "journalctl (log source)";
        if (has_journalctl()) {
            dr.status = CheckStatus::OK;
            dr.detail = "available";
        } else {
            dr.status = CheckStatus::SKIP;
            dr.detail = "not found — will read log file directly";
        }
        r.push_back(dr);
    }
    {
        DoctorResult dr;
        dr.name = "launchctl (macOS service manager)";
        if (has_launchctl()) {
            dr.status = CheckStatus::OK;
            dr.detail = "available";
        } else {
            dr.status = CheckStatus::SKIP;
            dr.detail = "not on PATH";
        }
        r.push_back(dr);
    }

    int any_credential = 0;
    for (const auto& [platform, env_var] : gateway_credential_env_vars()) {
        DoctorResult dr;
        dr.name = "credential: " + platform + " ($" + env_var + ")";
        const char* v = std::getenv(env_var.c_str());
        if (v && *v) {
            dr.status = CheckStatus::OK;
            dr.detail = "set";
            ++any_credential;
        } else {
            dr.status = CheckStatus::SKIP;
            dr.detail = "unset";
        }
        r.push_back(dr);
    }
    if (any_credential == 0) {
        DoctorResult dr;
        dr.name = "at least one gateway credential configured";
        dr.status = CheckStatus::WARN;
        dr.detail = "no gateway platform credentials are set — "
                    "the gateway will start but handle no traffic.";
        r.push_back(dr);
    }
    return r;
}

int doctor_exit_code(const std::vector<DoctorResult>& results) {
    for (const auto& rr : results) {
        if (rr.status == CheckStatus::FAIL) return 1;
    }
    return 0;
}

// ===========================================================================
// CLI commands
// ===========================================================================

namespace {

void print_help() {
    std::cout << "hermes gateway — manage the messaging gateway service\n\n"
              << "Usage:\n"
              << "  hermes gateway start [--profile NAME]\n"
              << "  hermes gateway stop  [--profile NAME] [--all]\n"
              << "  hermes gateway restart [--profile NAME]\n"
              << "  hermes gateway reload  [--profile NAME]\n"
              << "  hermes gateway status  [--profile NAME] [--verbose]\n"
              << "  hermes gateway reconnect\n"
              << "  hermes gateway logs [--follow] [--tail N] [--grep PAT]\n"
              << "                      [--regex PAT] [--level LVL]\n"
              << "                      [--since 'YYYY-MM-DD HH:MM:SS']\n"
              << "                      [--journald] [--invert]\n"
              << "  hermes gateway install   [--exec PATH] [--profile NAME]\n"
              << "                           [--launchd]\n"
              << "  hermes gateway uninstall [--profile NAME] [--launchd]\n"
              << "  hermes gateway pids      [--profile NAME] [--verbose]\n"
              << "  hermes gateway profiles\n"
              << "  hermes gateway doctor\n";
}

std::string status_glyph(CheckStatus s) {
    switch (s) {
        case CheckStatus::OK:   return col::green("\xe2\x9c\x93");
        case CheckStatus::WARN: return col::yellow("!");
        case CheckStatus::FAIL: return col::red("\xe2\x9c\x97");
        case CheckStatus::SKIP: return col::dim("\xc2\xb7");
    }
    return "?";
}

std::string format_uptime(std::uint64_t secs) {
    if (secs == 0) return "-";
    std::ostringstream os;
    if (secs >= 86400) os << (secs / 86400) << "d";
    else if (secs >= 3600) os << (secs / 3600) << "h";
    else if (secs >= 60)   os << (secs / 60) << "m";
    else os << secs << "s";
    return os.str();
}

// Read a CLI arg's value for `--key VALUE` or `--key=VALUE`.
std::string arg_value(const std::vector<std::string>& argv,
                      const std::string& key,
                      const std::string& fallback = "") {
    for (std::size_t i = 0; i < argv.size(); ++i) {
        if (argv[i] == key && i + 1 < argv.size()) return argv[i + 1];
        std::string prefix = key + "=";
        if (argv[i].rfind(prefix, 0) == 0) return argv[i].substr(prefix.size());
    }
    return fallback;
}

bool arg_flag(const std::vector<std::string>& argv, const std::string& key) {
    return std::find(argv.begin(), argv.end(), key) != argv.end();
}

}  // namespace

int cmd_start(const std::vector<std::string>& argv) {
    std::string profile = arg_value(argv, "--profile");
    std::cout << "Starting gateway"
              << (profile.empty() ? "" : " (profile=" + profile + ")")
              << "...\n";

    // Already running?
    auto existing = filter_by_profile(find_gateway_processes(), profile);
    if (!existing.empty()) {
        std::cout << "  Gateway already running — PID(s):";
        for (const auto& p : existing) std::cout << " " << p.pid;
        std::cout << "\n";
        return 0;
    }

    if (systemd_service_installed(profile) && has_systemctl()) {
        auto r = run_cmd("systemctl --user start " +
                         systemd_service_name(profile));
        if (r.code == 0) {
            std::cout << "  " << col::green("\xe2\x9c\x93")
                      << " Started " << systemd_service_name(profile) << "\n";
            return 0;
        }
        std::cerr << "  systemctl start failed.\n";
        return 1;
    }
    if (launchd_service_installed(profile) && has_launchctl()) {
        run_cmd("launchctl load " + launchd_plist_path_for(profile).string());
        std::cout << "  " << col::green("\xe2\x9c\x93")
                  << " Loaded launchd agent " << launchd_label(profile) << "\n";
        return 0;
    }
    std::cout << "  No service unit installed for profile='" << profile
              << "'. Run 'hermes gateway install"
              << (profile.empty() ? "" : " --profile " + profile) << "' first.\n";
    return 0;
}

int cmd_stop(const std::vector<std::string>& argv) {
    std::string profile = arg_value(argv, "--profile");
    bool all = arg_flag(argv, "--all");
    std::cout << "Stopping gateway"
              << (all ? " (all profiles)"
                      : (profile.empty() ? "" : " (profile=" + profile + ")"))
              << "...\n";
    if (systemd_service_installed(profile) && has_systemctl()) {
        run_cmd("systemctl --user stop " + systemd_service_name(profile));
    }
    if (launchd_service_installed(profile) && has_launchctl()) {
        run_cmd("launchctl unload " + launchd_plist_path_for(profile).string());
    }
    int killed = 0;
#ifndef _WIN32
    auto procs = find_gateway_processes();
    auto target = all ? procs : filter_by_profile(procs, profile);
    for (const auto& p : target) {
        if (::kill(p.pid, SIGTERM) == 0) ++killed;
    }
#endif
    std::cout << "  Stopped " << killed << " process(es).\n";
    return 0;
}

int cmd_restart(const std::vector<std::string>& argv) {
    cmd_stop(argv);
    return cmd_start(argv);
}

int cmd_reconnect(const std::vector<std::string>& argv) {
    std::string profile = arg_value(argv, "--profile");
#ifndef _WIN32
    int sent = 0;
    auto procs = find_gateway_processes();
    auto target = profile.empty() ? procs : filter_by_profile(procs, profile);
    for (const auto& p : target) {
        if (::kill(p.pid, SIGUSR1) == 0) ++sent;
    }
    std::cout << "  Sent SIGUSR1 to " << sent << " process(es).\n";
    return 0;
#else
    (void)profile;
    std::cerr << "  reconnect not supported on this platform.\n";
    return 1;
#endif
}

int cmd_reload(const std::vector<std::string>& argv) {
    std::string profile = arg_value(argv, "--profile");
#ifndef _WIN32
    // Prefer systemctl reload when the service is active.
    if (systemd_service_active(profile) && has_systemctl()) {
        auto r = run_cmd("systemctl --user reload " + systemd_service_name(profile));
        if (r.code == 0) {
            std::cout << "  " << col::green("\xe2\x9c\x93")
                      << " Reload signalled via systemctl.\n";
            return 0;
        }
    }
    int sent = 0;
    auto procs = find_gateway_processes();
    auto target = profile.empty() ? procs : filter_by_profile(procs, profile);
    for (const auto& p : target) {
        if (::kill(p.pid, SIGHUP) == 0) ++sent;
    }
    std::cout << "  Sent SIGHUP to " << sent << " process(es).\n";
    return 0;
#else
    (void)profile;
    std::cerr << "  reload not supported on this platform.\n";
    return 1;
#endif
}

int cmd_status(const std::vector<std::string>& argv) {
    std::string profile = arg_value(argv, "--profile");
    bool verbose = arg_flag(argv, "--verbose") || arg_flag(argv, "-v");

    auto all_procs = find_gateway_processes();
    auto procs = profile.empty() ? all_procs : filter_by_profile(all_procs, profile);

    if (procs.empty()) {
        std::cout << "  " << col::red("\xe2\x9c\x97")
                  << " Gateway is not running"
                  << (profile.empty() ? "." : " (profile=" + profile + ").")
                  << "\n";
    } else {
        std::cout << "  " << col::green("\xe2\x9c\x93")
                  << " Gateway running — PID(s):";
        for (const auto& p : procs) std::cout << " " << p.pid;
        std::cout << "\n";
        if (verbose) {
            for (const auto& p : procs) {
                std::cout << "    - pid=" << p.pid
                          << "  profile=" << (p.profile.empty() ? "(default)" : p.profile)
                          << "  rss=" << p.rss_kb << "kB"
                          << "  up=" << format_uptime(p.uptime_sec)
                          << "\n";
            }
        }
    }
    std::cout << "    systemd unit: "
              << (systemd_service_installed(profile) ? "installed" : "not installed")
              << "\n";
    if (systemd_service_installed(profile)) {
        std::cout << "    active:        "
                  << (systemd_service_active(profile) ? "yes" : "no") << "\n";
    }
    if (launchd_service_installed(profile)) {
        std::cout << "    launchd agent: installed ("
                  << (launchd_service_loaded(profile) ? "loaded" : "unloaded")
                  << ")\n";
    }
    auto log = log_file_path();
    std::error_code ec;
    if (fs::exists(log, ec)) {
        std::cout << "    log:           " << log << " ("
                  << fs::file_size(log, ec) << " bytes)\n";
    }
    return 0;
}

int cmd_logs(const std::vector<std::string>& argv) {
    auto opts = parse_logs_args(argv);
    if (opts.journald) {
        if (!has_journalctl()) {
            std::cerr << "  --journald requested but journalctl is not on PATH.\n";
            return 1;
        }
        std::ostringstream cmd;
        cmd << "journalctl --user -u hermes-gateway.service -n " << opts.tail;
        if (opts.follow) cmd << " -f";
        if (opts.filter.grep.has_value()) {
            cmd << " -g " << "'" << *opts.filter.grep << "'";
        }
        return std::system(cmd.str().c_str()) == 0 ? 0 : 1;
    }
    auto path = log_file_path();
    if (!fs::exists(path)) {
        std::cerr << "  No log file at " << path << "\n";
        return 1;
    }
    std::ifstream f(path);
    std::vector<std::string> lines;
    std::string line;
    while (std::getline(f, line)) lines.push_back(line);

    auto filtered = filter_log_lines(lines, opts.filter);
    std::size_t start = 0;
    if ((int)filtered.size() > opts.tail) start = filtered.size() - opts.tail;
    for (std::size_t i = start; i < filtered.size(); ++i) {
        std::cout << filtered[i] << "\n";
    }
    if (!opts.follow) return 0;

#ifndef _WIN32
    f.clear();
    f.seekg(0, std::ios::end);
    while (true) {
        std::string more;
        while (std::getline(f, more)) {
            if (log_line_matches(more, opts.filter)) {
                std::cout << more << "\n";
                std::cout.flush();
            }
        }
        f.clear();
        usleep(300 * 1000);
    }
#endif
    return 0;
}

int cmd_install(const std::vector<std::string>& argv) {
    std::string exec_path = arg_value(argv, "--exec", "/usr/bin/env hermes");
    std::string profile = arg_value(argv, "--profile");
    bool launchd = arg_flag(argv, "--launchd");

    if (launchd) {
        auto dir = launchd_dir();
        std::error_code ec;
        fs::create_directories(dir, ec);
        if (ec) {
            std::cerr << "  Failed to create " << dir << ": " << ec.message() << "\n";
            return 1;
        }
        auto path = launchd_plist_path_for(profile);
        auto body = profile.empty()
                      ? render_launchd_plist(launchd_label(), exec_path)
                      : render_launchd_plist_for_profile(exec_path, profile);
        if (!atomic_write(path, body)) {
            std::cerr << "  Failed to write " << path << "\n";
            return 1;
        }
        std::cout << "  Wrote " << path << "\n";
        if (has_launchctl()) {
            std::cout << "  Run: launchctl load " << path << "\n";
        }
        return 0;
    }

    auto dir = user_systemd_dir();
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (ec) {
        std::cerr << "  Failed to create " << dir << ": " << ec.message() << "\n";
        return 1;
    }
    auto path = service_unit_path_for(profile);
    std::string body = profile.empty()
                          ? render_service_unit(exec_path)
                          : render_service_unit_for_profile(exec_path, profile);
    if (!atomic_write(path, body)) {
        std::cerr << "  Failed to write " << path << "\n";
        return 1;
    }
    std::cout << "  Wrote " << path << "\n";
    if (has_systemctl()) {
        run_cmd("systemctl --user daemon-reload");
        std::cout << "  Run: systemctl --user enable --now "
                  << systemd_service_name(profile) << "\n";
    }
    return 0;
}

int cmd_uninstall(const std::vector<std::string>& argv) {
    std::string profile = arg_value(argv, "--profile");
    bool launchd = arg_flag(argv, "--launchd");

    if (launchd) {
        auto path = launchd_plist_path_for(profile);
        if (!fs::exists(path)) {
            std::cout << "  No launchd agent installed.\n";
            return 0;
        }
        if (has_launchctl()) {
            run_cmd("launchctl unload " + path.string());
        }
        std::error_code ec;
        fs::remove(path, ec);
        if (ec) {
            std::cerr << "  Failed to remove " << path << ": " << ec.message() << "\n";
            return 1;
        }
        std::cout << "  Removed " << path << "\n";
        return 0;
    }

    auto p = service_unit_path_for(profile);
    if (!fs::exists(p)) {
        std::cout << "  No service unit installed"
                  << (profile.empty() ? "." : " for profile=" + profile + ".")
                  << "\n";
        return 0;
    }
    if (has_systemctl()) {
        run_cmd("systemctl --user stop " + systemd_service_name(profile));
        run_cmd("systemctl --user disable " + systemd_service_name(profile));
    }
    std::error_code ec;
    fs::remove(p, ec);
    if (ec) {
        std::cerr << "  Failed to remove " << p << ": " << ec.message() << "\n";
        return 1;
    }
    std::cout << "  Removed " << p << "\n";
    if (has_systemctl()) {
        run_cmd("systemctl --user daemon-reload");
    }
    return 0;
}

int cmd_pids(const std::vector<std::string>& argv) {
    std::string profile = arg_value(argv, "--profile");
    bool has_profile_flag =
        std::any_of(argv.begin(), argv.end(), [](const std::string& a) {
            return a == "--profile" || a.rfind("--profile=", 0) == 0;
        });
    bool verbose = arg_flag(argv, "--verbose") || arg_flag(argv, "-v");

    auto procs = find_gateway_processes();
    if (has_profile_flag) {
        procs = filter_by_profile(procs, profile);
    }
    if (procs.empty()) {
        std::cout << "  (no gateway processes)\n";
        return 0;
    }
    for (const auto& p : procs) {
        if (verbose) {
            std::cout << p.pid
                      << "\t" << (p.profile.empty() ? "(default)" : p.profile)
                      << "\t" << p.rss_kb << "kB"
                      << "\t" << format_uptime(p.uptime_sec)
                      << "\t" << p.cmdline
                      << "\n";
        } else {
            std::cout << p.pid << "\n";
        }
    }
    return 0;
}

int cmd_profiles(const std::vector<std::string>& /*argv*/) {
    auto procs = find_gateway_processes();
    if (procs.empty()) {
        std::cout << "  (no running gateway processes)\n";
        return 0;
    }
    // Group by profile.
    std::vector<std::pair<std::string, std::vector<int>>> groups;
    auto find_group = [&](const std::string& profile)
        -> std::vector<int>* {
        for (auto& g : groups) if (g.first == profile) return &g.second;
        groups.push_back({profile, {}});
        return &groups.back().second;
    };
    for (const auto& p : procs) find_group(p.profile)->push_back(p.pid);
    std::sort(groups.begin(), groups.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });
    for (const auto& [profile, pids] : groups) {
        std::cout << "  " << (profile.empty() ? "(default)" : profile)
                  << " — " << pids.size() << " pid(s):";
        for (int pid : pids) std::cout << " " << pid;
        std::cout << "\n";
    }
    return 0;
}

int cmd_doctor(const std::vector<std::string>& /*argv*/) {
    auto results = run_doctor_checks();
    std::cout << "hermes gateway doctor\n";
    std::cout << std::string(40, '-') << "\n";
    for (const auto& r : results) {
        std::cout << "  " << status_glyph(r.status) << " "
                  << std::left << std::setw(48) << r.name.substr(0, 48)
                  << "  " << r.detail << "\n";
    }
    int fails = 0, warns = 0;
    for (const auto& r : results) {
        if (r.status == CheckStatus::FAIL) ++fails;
        if (r.status == CheckStatus::WARN) ++warns;
    }
    std::cout << std::string(40, '-') << "\n";
    std::cout << "Summary: " << fails << " fail(s), " << warns << " warning(s).\n";
    return doctor_exit_code(results);
}

int run(int argc, char* argv[]) {
    if (argc <= 2) {
        return cmd_status({});
    }
    std::string sub = argv[2];
    std::vector<std::string> rest;
    for (int i = 3; i < argc; ++i) rest.emplace_back(argv[i]);

    if (sub == "start")     return cmd_start(rest);
    if (sub == "stop")      return cmd_stop(rest);
    if (sub == "restart")   return cmd_restart(rest);
    if (sub == "reconnect") return cmd_reconnect(rest);
    if (sub == "reload")    return cmd_reload(rest);
    if (sub == "status")    return cmd_status(rest);
    if (sub == "logs")      return cmd_logs(rest);
    if (sub == "install")   return cmd_install(rest);
    if (sub == "uninstall") return cmd_uninstall(rest);
    if (sub == "pids")      return cmd_pids(rest);
    if (sub == "profiles")  return cmd_profiles(rest);
    if (sub == "doctor")    return cmd_doctor(rest);
    if (sub == "--help" || sub == "-h" || sub == "help") {
        print_help();
        return 0;
    }
    std::cerr << "Unknown gateway subcommand: " << sub << "\n";
    print_help();
    return 1;
}

}  // namespace hermes::cli::gateway_cmd
