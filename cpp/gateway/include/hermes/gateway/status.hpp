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

#ifdef _WIN32
#include <cstdint>
// Windows-only: returns FILETIME-as-uint64 creation time, nullopt on failure.
std::optional<uint64_t> get_process_start_time(int pid);
#endif

}  // namespace hermes::gateway
