#include "hermes/environments/spawn_via_env.hpp"

#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <sstream>
#include <string>

namespace hermes::environments {

namespace {

// POSIX single-quote shell escaping.  Matches the helper used by the
// other backend modules.
std::string sq(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 2);
    out.push_back('\'');
    for (char c : s) {
        if (c == '\'') {
            out += "'\\''";
        } else {
            out.push_back(c);
        }
    }
    out.push_back('\'');
    return out;
}

std::string generate_handle_id() {
    auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                  std::chrono::steady_clock::now().time_since_epoch())
                  .count();
    std::ostringstream oss;
    oss << "hbg_" << std::hex << ns;
    return oss.str();
}

std::string trim(const std::string& s) {
    std::size_t b = 0, e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    return s.substr(b, e - b);
}

// Parse a decimal integer, returning std::nullopt on any non-digit.
std::optional<int> parse_int(const std::string& s) {
    if (s.empty()) return std::nullopt;
    std::size_t idx = 0;
    try {
        int v = std::stoi(s, &idx);
        if (idx != s.size()) return std::nullopt;
        return v;
    } catch (...) {
        return std::nullopt;
    }
}

}  // namespace

BackgroundHandle spawn_via_env(BaseEnvironment& env,
                               const std::string& command,
                               const SpawnViaEnvOptions& opts) {
    BackgroundHandle h;
    h.handle_id = opts.handle_id.empty() ? generate_handle_id()
                                         : opts.handle_id;
    h.log_path = opts.temp_dir + "/hermes_bg_" + h.handle_id + ".log";
    h.pid_path = opts.temp_dir + "/hermes_bg_" + h.handle_id + ".pid";
    h.exit_path = opts.temp_dir + "/hermes_bg_" + h.handle_id + ".exit";

    std::ostringstream script;
    script << "mkdir -p " << sq(opts.temp_dir) << " && "
           << "( nohup bash -lc " << sq(command) << " > "
           << sq(h.log_path) << " 2>&1; "
           << "rc=$?; printf '%s\\n' \"$rc\" > " << sq(h.exit_path)
           << " ) & "
           << "echo $! > " << sq(h.pid_path) << " && cat "
           << sq(h.pid_path);

    ExecuteOptions exec_opts;
    exec_opts.cwd = opts.cwd;
    exec_opts.env_vars = opts.env_vars;
    exec_opts.timeout = opts.bootstrap_timeout;

    auto result = env.execute(script.str(), exec_opts);
    if (result.exit_code != 0) {
        h.error = result.stderr_text.empty() ? result.stdout_text
                                             : result.stderr_text;
        return h;
    }

    // stdout is the PID (possibly with trailing newline / log noise from
    // shell rc files).  Scan lines until we find one that parses as int.
    std::istringstream iss(result.stdout_text);
    std::string line;
    while (std::getline(iss, line)) {
        if (auto pid = parse_int(trim(line))) {
            h.pid = pid;
            break;
        }
    }
    if (!h.pid) {
        h.error = "could not parse PID from backend stdout";
    }
    return h;
}

std::optional<int> poll_background(BaseEnvironment& env,
                                   const BackgroundHandle& handle) {
    // If the exit file doesn't exist the process is still running.
    std::string cmd =
        "if [ -f " + sq(handle.exit_path) + " ]; then cat " +
        sq(handle.exit_path) + "; else echo __HERMES_RUNNING__; fi";
    ExecuteOptions opts;
    opts.timeout = std::chrono::seconds{5};
    auto r = env.execute(cmd, opts);
    if (r.exit_code != 0) return std::nullopt;
    auto t = trim(r.stdout_text);
    if (t == "__HERMES_RUNNING__") return std::nullopt;
    return parse_int(t);
}

std::string read_background_log(BaseEnvironment& env,
                                const BackgroundHandle& handle,
                                std::size_t max_bytes) {
    std::string cmd = "tail -c " + std::to_string(max_bytes) + " " +
                      sq(handle.log_path) + " 2>/dev/null || true";
    ExecuteOptions opts;
    opts.timeout = std::chrono::seconds{10};
    auto r = env.execute(cmd, opts);
    return r.stdout_text;
}

bool kill_background(BaseEnvironment& env,
                     const BackgroundHandle& handle,
                     bool force) {
    if (!handle.pid) return false;
    std::string sig = force ? "-KILL" : "-TERM";
    std::string cmd = "kill " + sig + " " + std::to_string(*handle.pid);
    ExecuteOptions opts;
    opts.timeout = std::chrono::seconds{5};
    auto r = env.execute(cmd, opts);
    return r.exit_code == 0;
}

}  // namespace hermes::environments
