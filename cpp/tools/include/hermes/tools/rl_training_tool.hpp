// Phase 8 / depth-port: RL training tools.
//
// Two complementary surfaces:
//
//   (1) register_rl_tools(transport) — thin HTTP client for a remote
//       Nous RL API.  Useful when running against a hosted controller.
//
//   (2) RlLocalRegistry — in-process mirror of
//       ``tools/rl_training_tool.py`` which scans a local
//       ``tinker-atropos`` checkout, tracks selected environment /
//       config / run state, and spawns training subprocesses.  The
//       registered tools dispatch to this registry when a transport is
//       not available *and* ``TINKER_ATROPOS_ROOT`` points at a real
//       directory.
//
// The registry keeps global mutable state because it mirrors the Python
// module's top-level globals — wrap mutations in its internal mutex.
#pragma once

#include "hermes/llm/llm_client.hpp"

#include <nlohmann/json.hpp>

#include <chrono>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace hermes::tools {

// ---------------------------------------------------------------------------
// Data types (mirror the Python dataclasses)
// ---------------------------------------------------------------------------

struct RlEnvironment {
    std::string name;
    std::string description;
    std::string version;
};

struct RlTrainingConfig {
    nlohmann::json params;
};

struct RlRunStatus {
    std::string run_id;
    std::string state;
    double progress = 0.0;
    nlohmann::json metrics;
};

struct EnvironmentInfo {
    std::string name;
    std::string class_name;
    std::string file_path;
    std::string description;
    std::string config_class = "BaseEnvConfig";
};

struct RunState {
    std::string run_id;
    std::string environment;
    nlohmann::json config;
    std::string status = "pending";  // pending|starting|running|stopping|stopped|completed|failed
    std::string error_message;
    std::string wandb_project;
    std::string wandb_run_name;
    std::chrono::system_clock::time_point start_time{};
    std::string config_path;   // YAML config on disk
    std::string api_log_path;
    std::string trainer_log_path;
    std::string env_log_path;
    // PIDs of spawned processes (0 = not spawned).  The C++ port does
    // not reap children in-process — the trainer script is expected to
    // handle its own lifecycle; we record PIDs so ``rl_stop_training``
    // can signal them.
    long api_pid = 0;
    long trainer_pid = 0;
    long env_pid = 0;
};

struct TestModel {
    std::string id;
    std::string name;
    std::string scale;
};

// Default test parameters — quick but representative.
inline constexpr int kDefaultNumSteps = 3;
inline constexpr int kDefaultGroupSize = 16;

// Rate-limit window for rl_check_status (30 minutes, mirrors Python).
inline constexpr int kMinStatusCheckIntervalSeconds = 30 * 60;

// ---------------------------------------------------------------------------
// Locked fields — infrastructure settings the model cannot override.
// Provided as JSON so the registry can serialise / diff against user
// config without having to hand-maintain a parallel struct.
// ---------------------------------------------------------------------------
nlohmann::json locked_fields();

// ---------------------------------------------------------------------------
// Local registry — in-process state mirror of the Python module.
// ---------------------------------------------------------------------------
class RlLocalRegistry {
public:
    static RlLocalRegistry& instance();

    // Path helpers — resolved from TINKER_ATROPOS_ROOT / HERMES_HOME.
    std::string tinker_root() const;
    std::string environments_dir() const;
    std::string configs_dir() const;
    std::string logs_dir() const;

    // Environment discovery — mirrors _scan_environments().
    std::vector<EnvironmentInfo> scan_environments() const;
    // Lazy cache: returns the scanned list, caching on first call.
    const std::vector<EnvironmentInfo>& environments();
    void invalidate_environment_cache();

    // Config field introspection — shallow, AST-free mirror of
    // _get_env_config_fields.  The C++ port cannot exec Python, so it
    // falls back to a static list of known BaseEnvConfig fields when the
    // Python helper is not reachable.
    nlohmann::json env_config_fields(const std::string& env_name);

    // Selected environment + in-progress config (mirrors _current_env /
    // _current_config).
    std::optional<std::string> current_env() const;
    nlohmann::json current_config() const;
    void set_current_env(const std::string& name);
    void set_current_config_field(const std::string& field,
                                  const nlohmann::json& value);
    void reset_current_config();

    // Run-state tracking.
    RunState* create_run(const std::string& env_name,
                         const nlohmann::json& config);
    RunState* get_run(const std::string& run_id);
    std::vector<RunState> list_runs() const;
    void set_run_status(const std::string& run_id,
                        const std::string& status,
                        const std::string& err = {});

    // Rate limiting — returns remaining seconds (0 if allowed).
    int status_check_cooldown(const std::string& run_id);
    void record_status_check(const std::string& run_id);

    // Test models for rl_test_inference.
    static const std::vector<TestModel>& test_models();

    // Clear everything (for tests).
    void reset();

private:
    RlLocalRegistry() = default;
    RlLocalRegistry(const RlLocalRegistry&) = delete;
    RlLocalRegistry& operator=(const RlLocalRegistry&) = delete;

    mutable std::mutex mu_;
    std::vector<EnvironmentInfo> env_cache_;
    bool env_cache_ready_ = false;
    std::map<std::string, nlohmann::json> env_fields_cache_;

    std::optional<std::string> current_env_;
    nlohmann::json current_config_ = nlohmann::json::object();

    std::map<std::string, RunState> active_runs_;
    std::map<std::string, std::chrono::system_clock::time_point> last_check_;
};

// ---------------------------------------------------------------------------
// Helpers exposed for unit tests.
// ---------------------------------------------------------------------------

// Parse a Python environment source and return the BaseEnv subclass name
// (with docstring + config_class).  Text-based, intentionally lenient.
std::optional<EnvironmentInfo> parse_env_file(const std::string& path);

// Build a YAML representation of a run config, merging locked_fields()
// and the user-supplied ``env`` / ``tinker`` keys.
std::string build_run_yaml(const std::string& env_name,
                           const nlohmann::json& user_config,
                           const std::string& wandb_project,
                           const std::string& wandb_run_name);

// Check whether ``key`` is missing from the environment.  Returns the
// empty string when all keys are present, else the name of the first
// missing one.
std::string first_missing_env(const std::vector<std::string>& keys);

// ---------------------------------------------------------------------------
// Tool registration.
// ---------------------------------------------------------------------------

// Registers all rl_* tools against the global ToolRegistry.  When
// ``transport`` is null, the tools dispatch only against the local
// registry; otherwise they call the HTTP endpoints.
void register_rl_tools(hermes::llm::HttpTransport* transport);

}  // namespace hermes::tools
