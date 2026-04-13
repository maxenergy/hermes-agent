// SWE task executor — runs a SWE-bench-style task (problem statement +
// patch + test patch) inside a user-supplied environment and captures a
// pass/fail trajectory.
//
// The runner itself is environment-agnostic: it takes a pointer to a
// ``hermes::environments::BaseEnvironment`` and issues shell commands
// through it.  Tests can inject a mock environment.  In production the
// caller picks ``local`` / ``docker`` / ``modal`` from the JSONL task
// spec.
#pragma once

#include <chrono>
#include <filesystem>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "hermes/environments/base.hpp"

namespace hermes::batch {

struct SweTask {
    std::string task_id;
    std::string problem_statement;
    std::filesystem::path repo_path;
    std::string test_patch;   // unified diff for the test files
    std::string model_patch;  // unified diff produced by the agent (optional)
    std::string test_cmd = "pytest -x";
    std::chrono::seconds timeout{1800};
};

struct SweResult {
    std::string task_id;
    bool model_patch_applied = false;
    bool test_patch_applied = false;
    bool tests_passed = false;
    int test_exit_code = -1;
    std::string test_stdout;
    std::string test_stderr;
    std::chrono::milliseconds duration{0};
    std::string error;  // populated on setup failure
};

class SweRunner {
public:
    explicit SweRunner(environments::BaseEnvironment* env);

    /// Run the full pipeline: apply model_patch, apply test_patch,
    /// execute test_cmd.  Each step runs through the injected env so
    /// ``docker``/``modal`` tasks execute in isolation.
    SweResult run(const SweTask& task);

    /// Serialize a result as an HF SFT record where:
    ///   system = "Solve a SWE-bench task ..."
    ///   human  = problem_statement
    ///   gpt    = model_patch
    ///   tool   = pass/fail summary
    nlohmann::json to_hf_record(const SweTask& task, const SweResult& result) const;

private:
    environments::BaseEnvironment* env_;
};

}  // namespace hermes::batch
