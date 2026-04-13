// BatchRunner — runs a JSONL dataset through AIAgent with a thread pool.
#pragma once

#include <chrono>
#include <filesystem>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace hermes::batch {

struct BatchConfig {
    std::filesystem::path dataset_path;  // JSONL input
    std::filesystem::path output_dir;
    int num_workers = 4;
    std::string model;
    std::vector<std::string> enabled_toolsets;
    std::string distribution = "default";  // toolset distribution name
    bool save_trajectories = true;

    // Default environment for tasks that don't specify one.  Recognised
    // values: ``local``, ``docker``, ``modal`` (additional backends
    // available via ``cpp/environments``).  Per-task JSONL may override
    // with an ``environment`` field.
    std::string default_environment = "local";

    // Default task type.  ``prompt`` (single-turn SFT) or ``swe`` (apply
    // patch + run tests).  Per-task JSONL may override with a ``type``
    // field.
    std::string default_task_type = "prompt";

    // Checkpoint + progress reporting cadence.  Progress JSON is emitted
    // to stderr every ``progress_interval``; a soft state snapshot is
    // written every ``checkpoint_interval`` (used for RL long-tasks).
    std::chrono::seconds progress_interval{60};
    std::chrono::seconds checkpoint_interval{30 * 60};
};

struct BatchResult {
    int total_prompts = 0;
    int completed = 0;
    int failed = 0;
    int swe_passed = 0;  // subset of completed, for SWE tasks
    int swe_failed = 0;  // completed SWE tasks that did not pass
    std::map<std::string, int> tool_stats;  // tool_name -> call count
    std::chrono::milliseconds duration{0};
};

/// Snapshot of a running batch — used for progress reporting + 30-min
/// soft checkpoints during RL training loops.
struct BatchProgress {
    int total = 0;
    int completed = 0;
    int failed = 0;
    std::chrono::milliseconds elapsed{0};
    bool is_final = false;
    nlohmann::json to_json() const;
};

class BatchRunner {
public:
    using ProgressCallback = std::function<void(const BatchProgress&)>;

    explicit BatchRunner(BatchConfig config);

    // Run all prompts from dataset. Uses thread pool.
    BatchResult run();

    // Resume from checkpoint
    BatchResult resume();

    /// Install a callback invoked at ``progress_interval`` and once at
    /// completion.  The callback runs on the main thread (in
    /// ``run()``), so it must not block for long.
    void set_progress_callback(ProgressCallback cb) {
        progress_cb_ = std::move(cb);
    }

    /// Resolve the environment name for a given task JSON — per-task
    /// ``environment`` override wins, otherwise the config default.
    /// Exposed for testing / reuse from the RL CLI.
    std::string resolve_env(const nlohmann::json& task) const;

    /// Resolve the task type (``prompt`` | ``swe``) for a given task.
    std::string resolve_type(const nlohmann::json& task) const;

private:
    BatchConfig config_;
    ProgressCallback progress_cb_;
    // Checkpoint: {output_dir}/checkpoint.json tracks completed prompt indices
    void save_checkpoint(int completed_index);
    int load_checkpoint();
    void emit_progress(const BatchProgress& p) const;
};

}  // namespace hermes::batch
