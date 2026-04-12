#include <hermes/gateway/status.hpp>

#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <unistd.h>
#include <signal.h>

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
    return kill(pid, 0) == 0;
}

}  // namespace

void write_pid_file() {
    auto path = get_pid_file_path();
    fs::create_directories(path.parent_path());
    std::ofstream out(path);
    out << getpid();
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
            if (existing_pid > 0 && process_exists(existing_pid)) {
                return false;  // Lock held by live process.
            }
        } catch (...) {
            // Corrupted lock file — remove and take lock.
        }
    }

    nlohmann::json lock_data = {
        {"pid", static_cast<int>(getpid())},
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
    if (force) {
        kill(pid, SIGKILL);
    } else {
        kill(pid, SIGTERM);
    }
}

}  // namespace hermes::gateway
