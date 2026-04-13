// PID detection + scoped locks + runtime status.
#pragma once

#include <chrono>
#include <map>
#include <optional>
#include <string>

#include <nlohmann/json.hpp>

#include <hermes/gateway/gateway_config.hpp>

namespace hermes::gateway {

// PID file management
void write_pid_file();
std::optional<int> read_pid_file();
bool is_gateway_running();

// Scoped locks — prevent two profiles using same credential
bool acquire_scoped_lock(const std::string& scope,
                         const std::string& identity,
                         const nlohmann::json& metadata = {});
void release_scoped_lock(const std::string& scope,
                         const std::string& identity);

// Runtime status persistence
struct RuntimeStatus {
    std::string state;  // starting|running|fatal|stopping
    std::string exit_reason;
    bool restart_requested = false;
    std::map<Platform, std::string>
        platform_states;  // connected|disconnected|fatal
    std::chrono::system_clock::time_point timestamp;
};

void write_runtime_status(const RuntimeStatus& status);
std::optional<RuntimeStatus> read_runtime_status();

// Process termination
void terminate_pid(int pid, bool force = false);

// Returns true when the live PID still looks like the Hermes gateway.
// Inspects /proc/<pid>/cmdline on Linux, `ps -o args=` on macOS,
// and QueryFullProcessImageNameW + GetCommandLineW heuristics on Windows.
// Mirrors gateway/status.py:_looks_like_gateway_process.
bool looks_like_gateway_process(int pid);

// Cross-platform process start time as a wall-clock time_point.
//   Linux:   /proc/<pid>/stat field 22 (clock ticks since boot) + uptime.
//   macOS:   sysctl(KERN_PROC_PID).kp_proc.p_starttime.
//   Windows: GetProcessTimes() CreationTime FILETIME -> system_clock.
// Returns nullopt when the lookup fails (process gone, no permission,
// platform unsupported).
std::optional<std::chrono::system_clock::time_point>
get_process_start_time(int pid);

#ifdef _WIN32
#include <cstdint>
// Windows-only: returns FILETIME-as-uint64 creation time, nullopt on failure.
// Retained for binary identity checks done elsewhere; prefer the
// time_point overload above for new code.
std::optional<uint64_t> get_process_start_time_filetime(int pid);
#endif

}  // namespace hermes::gateway
