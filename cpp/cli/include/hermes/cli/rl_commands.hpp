// Dedicated RL training CLI — ``hermes rl train`` / ``hermes rl eval``.
//
// This is a thin wrapper around ``hermes::batch::BatchRunner`` with a
// config tailored for reinforcement-learning workflows:
//   - extended iteration / timeout budgets
//   - RL-focused system prompt
//   - full toolset including the Nous RL training tools
//   - 30-min soft checkpoint via BatchRunner progress watchdog
//
// Python reference: ``rl_cli.py``.
#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace hermes::cli::rl {

struct RlConfig {
    std::filesystem::path dataset_path;   // JSONL — same shape as batch
    std::filesystem::path output_dir;
    std::string model = "anthropic/claude-opus-4.5";
    std::string base_url;                  // optional override
    std::string default_environment = "local";
    int max_iterations = 200;
    int num_workers = 1;                   // serial by default for RL
    std::vector<std::string> enabled_toolsets = {"terminal", "web", "rl"};

    // RL-specific system prompt injected as the first ``system`` turn
    // in every trajectory.  Default value matches the Python CLI
    // prompt in ``rl_cli.py``.
    std::string system_prompt;

    // Cadence in seconds.
    int progress_interval_seconds = 60;
    int checkpoint_interval_seconds = 30 * 60;

    bool eval_only = false;                // --eval → skip training tools
    bool save_trajectories = true;
};

/// Canonical RL system prompt (mirror of RL_SYSTEM_PROMPT in
/// ``rl_cli.py``).
std::string default_rl_system_prompt();

/// Canonical toolset list for RL workflows.  Always includes the
/// ``rl`` toolset which registers ``rl_list_environments`` /
/// ``rl_start_training`` / etc.  Eval mode drops the training-mutation
/// tools (start / stop / edit_config) and keeps inference-only tools.
std::vector<std::string> rl_toolset_names(bool eval_only);

/// Load an RL YAML/JSON config file into ``RlConfig``.  Missing keys
/// keep their defaults; unknown keys are ignored.  Recognised keys:
///   dataset, output_dir, model, base_url, environment, max_iterations,
///   num_workers, toolsets, system_prompt, progress_interval,
///   checkpoint_interval, eval_only, save_trajectories.
RlConfig load_rl_config(const std::filesystem::path& path);

/// Parse the rl subcommand arguments.  Returns a CLI exit code.
/// argv[0] is the verb (``train`` | ``eval`` | ``list-environments``
/// | ``--help``).  Out-of-band errors are printed to stderr.
int dispatch_rl_command(const std::vector<std::string>& argv);

/// Execute the RL loop using ``BatchRunner``.  Returns the number of
/// completed tasks.  ``dataset`` is expected to contain one JSON task
/// per line; empty dataset means "train without a prompt feed" and
/// degrades to a single no-op invocation.
int run_rl_training(const RlConfig& config);

/// Inference-only counterpart of ``run_rl_training``.  Disables the
/// mutation-style RL tools.
int run_rl_eval(const RlConfig& config);

}  // namespace hermes::cli::rl
