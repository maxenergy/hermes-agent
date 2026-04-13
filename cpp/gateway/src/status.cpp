#include <hermes/gateway/status.hpp>

#include <array>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <string>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <process.h>
#elif defined(__APPLE__)
#include <unistd.h>
#include <signal.h>
#include <sys/sysctl.h>
#include <sys/types.h>
#else
#include <unistd.h>
#include <signal.h>
#endif

#include <openssl/sha.h>

#include <hermes/core/path.hpp>

namespace hermes::gateway {

namespace {

namespace fs = std::filesystem;

fs::path get_gateway_state_dir() {
    const char* custom = std::getenv("HERMES_GATEWAY_LOCK_DIR");
    if (custom && custom[0] != '\0') {
        return fs::path(custom);
    }
    const char* xdg = std::getenv("XDG_STATE_HOME");
    if (xdg && xdg[0] != '\0') {
        return fs::path(xdg) / "hermes" / "gateway-locks";
    }
    const char* home = std::getenv("HOME");
    if (home) {
        return fs::path(home) / ".local" / "state" / "hermes" /
               "gateway-locks";
    }
    return fs::path("/tmp") / "hermes-gateway-locks";
}

fs::path get_pid_file_path() {
    return hermes::core::path::get_hermes_home() / "gateway" /
           "gateway.pid";
}

fs::path get_status_file_path() {
    return hermes::core::path::get_hermes_home() / "gateway" /
           "status.json";
}

std::string sha256_hex_prefix(const std::string& input, int chars) {
    unsigned char hash[SHA256_DIGEST_LENGTH];
    SHA256(reinterpret_cast<const unsigned char*>(input.data()),
           input.size(), hash);

    int bytes = (chars + 1) / 2;
    std::ostringstream oss;
    for (int i = 0; i < bytes; ++i) {
        oss << std::hex << std::setfill('0') << std::setw(2)
            << static_cast<int>(hash[i]);
    }
    auto s = oss.str();
    return s.substr(0, static_cast<size_t>(chars));
}

std::string time_to_iso(std::chrono::system_clock::time_point tp) {
    auto t = std::chrono::system_clock::to_time_t(tp);
    std::tm tm{};
    gmtime_r(&t, &tm);
    std::ostringstream oss;
    oss << std::put_time(&tm, "%Y-%m-%dT%H:%M:%SZ");
    return oss.str();
}

std::chrono::system_clock::time_point iso_to_time(const std::string& s) {
    std::tm tm{};
    std::istringstream iss(s);
    iss >> std::get_time(&tm, "%Y-%m-%dT%H:%M:%SZ");
    return std::chrono::system_clock::from_time_t(timegm(&tm));
}

bool process_exists(int pid) {
#ifdef _WIN32
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, (DWORD)pid);
    if (!h) return false;
    DWORD exit_code = 0;
    bool alive = GetExitCodeProcess(h, &exit_code) && exit_code == STILL_ACTIVE;
    CloseHandle(h);
    return alive;
#else
    return kill(pid, 0) == 0;
#endif
}

int current_pid() {
#ifdef _WIN32
    return static_cast<int>(GetCurrentProcessId());
#else
    return static_cast<int>(getpid());
#endif
}

}  // namespace

void write_pid_file() {
    auto path = get_pid_file_path();
    fs::create_directories(path.parent_path());
    std::ofstream out(path);
    out << current_pid();
}

std::optional<int> read_pid_file() {
    auto path = get_pid_file_path();
    if (!fs::exists(path)) return std::nullopt;

    std::ifstream in(path);
    int pid = 0;
    in >> pid;
    if (pid <= 0) return std::nullopt;
    return pid;
}

bool is_gateway_running() {
    auto pid = read_pid_file();
    if (!pid) return false;
    return process_exists(*pid);
}

bool acquire_scoped_lock(const std::string& scope,
                         const std::string& identity,
                         const nlohmann::json& metadata) {
    auto dir = get_gateway_state_dir();
    fs::create_directories(dir);

    auto hash = sha256_hex_prefix(identity, 16);
    auto lock_path = dir / (scope + "-" + hash + ".lock");

    // Check if lock exists and process is alive.
    if (fs::exists(lock_path)) {
        std::ifstream in(lock_path);
        nlohmann::json j;
        try {
            in >> j;
            int existing_pid = j.value("pid", 0);
            if (existing_pid > 0 && process_exists(existing_pid) &&
                existing_pid != current_pid()) {
                return false;  // Lock held by a DIFFERENT live process.
            }
            // If existing_pid == our pid, we just re-take the lock (idempotent).
        } catch (...) {
            // Corrupted lock file — remove and take lock.
        }
    }

    nlohmann::json lock_data = {
        {"pid", current_pid()},
        {"start_time", time_to_iso(std::chrono::system_clock::now())},
        {"scope", scope},
        {"identity", identity},
        {"metadata", metadata},
    };

    std::ofstream out(lock_path);
    out << lock_data.dump(2);
    return true;
}

void release_scoped_lock(const std::string& scope,
                         const std::string& identity) {
    auto dir = get_gateway_state_dir();
    auto hash = sha256_hex_prefix(identity, 16);
    auto lock_path = dir / (scope + "-" + hash + ".lock");

    if (fs::exists(lock_path)) {
        fs::remove(lock_path);
    }
}

void write_runtime_status(const RuntimeStatus& status) {
    auto path = get_status_file_path();
    fs::create_directories(path.parent_path());

    nlohmann::json j;
    j["state"] = status.state;
    j["exit_reason"] = status.exit_reason;
    j["restart_requested"] = status.restart_requested;
    j["timestamp"] = time_to_iso(status.timestamp);

    nlohmann::json ps = nlohmann::json::object();
    for (auto& [p, s] : status.platform_states) {
        ps[platform_to_string(p)] = s;
    }
    j["platform_states"] = ps;

    std::ofstream out(path);
    out << j.dump(2);
}

std::optional<RuntimeStatus> read_runtime_status() {
    auto path = get_status_file_path();
    if (!fs::exists(path)) return std::nullopt;

    std::ifstream in(path);
    nlohmann::json j;
    try {
        in >> j;
    } catch (...) {
        return std::nullopt;
    }

    RuntimeStatus status;
    status.state = j.value("state", "");
    status.exit_reason = j.value("exit_reason", "");
    status.restart_requested = j.value("restart_requested", false);
    status.timestamp = iso_to_time(j.value("timestamp", ""));

    if (j.contains("platform_states")) {
        for (auto& [k, v] : j["platform_states"].items()) {
            try {
                status.platform_states[platform_from_string(k)] =
                    v.get<std::string>();
            } catch (...) {
                // Skip unknown platforms.
            }
        }
    }

    return status;
}

void terminate_pid(int pid, bool force) {
    if (pid <= 0) return;
#ifdef _WIN32
    HANDLE h = OpenProcess(PROCESS_TERMINATE, FALSE, (DWORD)pid);
    if (h) {
        TerminateProcess(h, force ? 9 : 0);
        CloseHandle(h);
    }
#else
    if (force) {
        kill(pid, SIGKILL);
    } else {
        kill(pid, SIGTERM);
    }
#endif
}

#ifdef _WIN32
std::optional<uint64_t> get_process_start_time_filetime(int pid) {
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, (DWORD)pid);
    if (!h) return std::nullopt;
    FILETIME ct, et, kt, ut;
    bool ok = GetProcessTimes(h, &ct, &et, &kt, &ut);
    CloseHandle(h);
    if (!ok) return std::nullopt;
    ULARGE_INTEGER u;
    u.LowPart = ct.dwLowDateTime;
    u.HighPart = ct.dwHighDateTime;
    return u.QuadPart;
}
#endif

namespace {

// Heuristic shared by all platforms.  Mirrors the Python tuple of
// accepted patterns in gateway/status.py:_looks_like_gateway_process.
// Substrings like "hermes_gateway_tests" intentionally do NOT match --
// we require either the literal "hermes gateway" subcommand form or
// the "gateway/run.py" entry-point path.
bool cmdline_looks_like_gateway(const std::string& cmdline) {
    if (cmdline.empty()) return false;
    static const std::array<const char*, 4> patterns = {
        "hermes_cli.main gateway",
        "hermes_cli/main.py gateway",
        "hermes gateway",
        "gateway/run.py",
    };
    for (auto* needle : patterns) {
        if (cmdline.find(needle) != std::string::npos) return true;
    }
    return false;
}

#ifdef __linux__
std::optional<std::string> read_proc_cmdline(int pid) {
    auto path = std::string("/proc/") + std::to_string(pid) + "/cmdline";
    std::ifstream in(path, std::ios::binary);
    if (!in) return std::nullopt;
    std::string raw((std::istreambuf_iterator<char>(in)),
                    std::istreambuf_iterator<char>());
    if (raw.empty()) return std::nullopt;
    // /proc cmdline uses NUL separators -- replace with spaces.
    for (auto& c : raw) {
        if (c == '\0') c = ' ';
    }
    // Trim trailing whitespace.
    while (!raw.empty() && std::isspace(static_cast<unsigned char>(raw.back()))) {
        raw.pop_back();
    }
    return raw;
}
#endif

#if defined(__APPLE__) || defined(__linux__)
// `ps -o args= -p PID` works on both macOS and most Linux distros and is
// our portable fallback when /proc isn't available.
std::optional<std::string> read_ps_cmdline(int pid) {
    std::string cmd = "ps -o args= -p " + std::to_string(pid) + " 2>/dev/null";
    FILE* fp = popen(cmd.c_str(), "r");
    if (!fp) return std::nullopt;
    std::string out;
    char buf[1024];
    while (std::fgets(buf, sizeof(buf), fp)) {
        out.append(buf);
    }
    pclose(fp);
    while (!out.empty() && (out.back() == '\n' || out.back() == ' ')) {
        out.pop_back();
    }
    if (out.empty()) return std::nullopt;
    return out;
}
#endif

}  // namespace

bool looks_like_gateway_process(int pid) {
    if (pid <= 0) return false;
#ifdef _WIN32
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, (DWORD)pid);
    if (!h) return false;
    wchar_t buf[MAX_PATH];
    DWORD sz = MAX_PATH;
    bool ok = QueryFullProcessImageNameW(h, 0, buf, &sz) != 0;
    CloseHandle(h);
    if (!ok) return false;
    // Convert WCHAR -> std::string (best-effort UTF-16 -> UTF-8).
    std::string narrow;
    narrow.reserve(sz);
    for (DWORD i = 0; i < sz; ++i) {
        narrow.push_back(buf[i] < 128 ? static_cast<char>(buf[i]) : '?');
    }
    // QueryFullProcessImageNameW returns the executable path only.  The
    // command-line argv (with "gateway" subcommand) lives in the PEB and
    // is not retrievable without NtQueryInformationProcess; treat the
    // executable path as authoritative.
    return cmdline_looks_like_gateway(narrow);
#elif defined(__linux__)
    if (auto cmd = read_proc_cmdline(pid)) {
        return cmdline_looks_like_gateway(*cmd);
    }
    if (auto cmd = read_ps_cmdline(pid)) {
        return cmdline_looks_like_gateway(*cmd);
    }
    return false;
#elif defined(__APPLE__)
    if (auto cmd = read_ps_cmdline(pid)) {
        return cmdline_looks_like_gateway(*cmd);
    }
    return false;
#else
    (void)pid;
    return false;
#endif
}

std::optional<std::chrono::system_clock::time_point>
get_process_start_time(int pid) {
    if (pid <= 0) return std::nullopt;
#ifdef _WIN32
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, (DWORD)pid);
    if (!h) return std::nullopt;
    FILETIME ct, et, kt, ut;
    bool ok = GetProcessTimes(h, &ct, &et, &kt, &ut) != 0;
    CloseHandle(h);
    if (!ok) return std::nullopt;
    // FILETIME is 100-ns intervals since 1601-01-01; system_clock epoch is
    // 1970-01-01.  Difference: 11644473600 seconds -> 116444736000000000
    // hundred-nanosecond ticks.
    ULARGE_INTEGER u;
    u.LowPart = ct.dwLowDateTime;
    u.HighPart = ct.dwHighDateTime;
    constexpr uint64_t epoch_diff = 116444736000000000ULL;
    if (u.QuadPart < epoch_diff) return std::nullopt;
    auto unix_100ns = u.QuadPart - epoch_diff;
    // Convert 100-ns ticks to system_clock duration.
    auto secs = std::chrono::seconds(static_cast<int64_t>(unix_100ns / 10000000ULL));
    auto rem_100ns = static_cast<int64_t>(unix_100ns % 10000000ULL);
    auto nsec = std::chrono::nanoseconds(rem_100ns * 100);
    return std::chrono::system_clock::time_point(secs) +
           std::chrono::duration_cast<std::chrono::system_clock::duration>(nsec);
#elif defined(__linux__)
    // /proc/<pid>/stat field 22 = clock ticks since boot.
    auto stat_path = std::string("/proc/") + std::to_string(pid) + "/stat";
    std::ifstream stat(stat_path);
    if (!stat) return std::nullopt;
    std::string content((std::istreambuf_iterator<char>(stat)),
                        std::istreambuf_iterator<char>());
    // The comm (field 2) is wrapped in parentheses and may contain spaces;
    // skip past the closing ')' before splitting.
    auto rparen = content.rfind(')');
    if (rparen == std::string::npos || rparen + 1 >= content.size()) {
        return std::nullopt;
    }
    std::istringstream iss(content.substr(rparen + 1));
    // Now we are positioned at field 3.  Field 22 is the 20th token from here.
    std::string tok;
    long long starttime_ticks = -1;
    for (int i = 3; i <= 22; ++i) {
        if (!(iss >> tok)) return std::nullopt;
        if (i == 22) {
            try {
                starttime_ticks = std::stoll(tok);
            } catch (...) {
                return std::nullopt;
            }
        }
    }
    if (starttime_ticks < 0) return std::nullopt;

    long ticks_per_sec = sysconf(_SC_CLK_TCK);
    if (ticks_per_sec <= 0) ticks_per_sec = 100;

    // /proc/uptime first column = seconds since boot (double).
    std::ifstream up("/proc/uptime");
    if (!up) return std::nullopt;
    double uptime_sec = 0.0;
    if (!(up >> uptime_sec)) return std::nullopt;

    double process_age_sec =
        uptime_sec - (static_cast<double>(starttime_ticks) /
                      static_cast<double>(ticks_per_sec));
    auto now = std::chrono::system_clock::now();
    auto start = now - std::chrono::milliseconds(
                           static_cast<int64_t>(process_age_sec * 1000.0));
    return start;
#elif defined(__APPLE__)
    // sysctl KERN_PROC_PID -> struct kinfo_proc (kp_proc.p_starttime).
    int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PID, pid};
    struct kinfo_proc kp{};
    size_t len = sizeof(kp);
    if (sysctl(mib, 4, &kp, &len, nullptr, 0) != 0 || len == 0) {
        return std::nullopt;
    }
    auto secs = std::chrono::seconds(kp.kp_proc.p_starttime.tv_sec);
    auto usecs = std::chrono::microseconds(kp.kp_proc.p_starttime.tv_usec);
    return std::chrono::system_clock::time_point(secs) +
           std::chrono::duration_cast<std::chrono::system_clock::duration>(usecs);
#else
    // TODO(unsupported-platform): no implementation; return nullopt.
    (void)pid;
    return std::nullopt;
#endif
}

}  // namespace hermes::gateway
