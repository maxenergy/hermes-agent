#include "hermes/batch/swe_runner.hpp"

#include <cstdlib>
#include <fstream>
#include <sstream>

namespace hermes::batch {

namespace {

// Write a diff payload to a temporary file and return its path.  The
// file is placed under the repo so it is visible to remote/Docker
// workspaces that bind-mount the repo directory.
std::filesystem::path write_diff(const std::filesystem::path& repo,
                                  const std::string& label,
                                  const std::string& diff) {
    auto path = repo / (label + ".patch");
    std::ofstream out(path);
    out << diff;
    return path;
}

environments::CompletedProcess run_cmd(environments::BaseEnvironment* env,
                                        const std::string& cmd,
                                        const std::filesystem::path& cwd,
                                        std::chrono::seconds timeout) {
    environments::ExecuteOptions opts;
    opts.cwd = cwd;
    opts.timeout = timeout;
    return env->execute(cmd, opts);
}

}  // namespace

SweRunner::SweRunner(environments::BaseEnvironment* env) : env_(env) {}

SweResult SweRunner::run(const SweTask& task) {
    SweResult r;
    r.task_id = task.task_id;
    auto t0 = std::chrono::steady_clock::now();

    if (env_ == nullptr) {
        r.error = "no environment injected";
        return r;
    }
    if (!std::filesystem::exists(task.repo_path)) {
        r.error = "repo_path does not exist: " + task.repo_path.string();
        return r;
    }

    // Apply model_patch first (if any).  Use ``git apply`` so the patch
    // fails fast on rejects.
    if (!task.model_patch.empty()) {
        auto p = write_diff(task.repo_path, "model", task.model_patch);
        auto rc = run_cmd(env_, "git apply --whitespace=nowarn " + p.string(),
                          task.repo_path, task.timeout);
        r.model_patch_applied = (rc.exit_code == 0);
        if (!r.model_patch_applied) {
            r.error = "model_patch failed to apply: " + rc.stderr_text;
            auto t1 = std::chrono::steady_clock::now();
            r.duration = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0);
            return r;
        }
    } else {
        r.model_patch_applied = true;
    }

    // Apply test_patch.  SWE-bench ships the test patch separately so we
    // can verify the fix against a pinned test version.
    if (!task.test_patch.empty()) {
        auto p = write_diff(task.repo_path, "test", task.test_patch);
        auto rc = run_cmd(env_, "git apply --whitespace=nowarn " + p.string(),
                          task.repo_path, task.timeout);
        r.test_patch_applied = (rc.exit_code == 0);
        if (!r.test_patch_applied) {
            r.error = "test_patch failed to apply: " + rc.stderr_text;
            auto t1 = std::chrono::steady_clock::now();
            r.duration = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0);
            return r;
        }
    } else {
        r.test_patch_applied = true;
    }

    // Execute tests.
    auto rc = run_cmd(env_, task.test_cmd, task.repo_path, task.timeout);
    r.test_exit_code = rc.exit_code;
    r.test_stdout = rc.stdout_text;
    r.test_stderr = rc.stderr_text;
    r.tests_passed = (rc.exit_code == 0 && !rc.timed_out);

    auto t1 = std::chrono::steady_clock::now();
    r.duration = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0);
    return r;
}

nlohmann::json SweRunner::to_hf_record(const SweTask& task,
                                        const SweResult& result) const {
    nlohmann::json conversations = nlohmann::json::array();

    conversations.push_back(
        {{"from", "system"},
         {"value", "Solve a SWE-bench task by producing a unified diff that "
                    "makes the failing tests pass."}});
    conversations.push_back(
        {{"from", "human"}, {"value", task.problem_statement}});

    std::string gpt_body = "<think>\n</think>\n";
    if (!task.model_patch.empty()) {
        gpt_body += "<tool_call>\n";
        nlohmann::json tc;
        tc["name"] = "apply_patch";
        tc["arguments"] = {{"patch", task.model_patch}};
        gpt_body += tc.dump();
        gpt_body += "\n</tool_call>";
    }
    conversations.push_back({{"from", "gpt"}, {"value", gpt_body}});

    nlohmann::json tool_content;
    tool_content["tests_passed"] = result.tests_passed;
    tool_content["exit_code"] = result.test_exit_code;
    tool_content["stdout_tail"] = result.test_stdout.size() > 4096
        ? result.test_stdout.substr(result.test_stdout.size() - 4096)
        : result.test_stdout;
    tool_content["stderr_tail"] = result.test_stderr.size() > 4096
        ? result.test_stderr.substr(result.test_stderr.size() - 4096)
        : result.test_stderr;

    nlohmann::json tool_wrapper;
    tool_wrapper["tool_call_id"] = task.task_id;
    tool_wrapper["name"] = "apply_patch";
    tool_wrapper["content"] = tool_content;

    std::ostringstream tool_value;
    tool_value << "<tool_response>\n" << tool_wrapper.dump()
               << "\n</tool_response>";
    conversations.push_back({{"from", "tool"}, {"value", tool_value.str()}});

    nlohmann::json record;
    record["conversations"] = conversations;
    record["metadata"] = {
        {"task_id", task.task_id},
        {"tests_passed", result.tests_passed},
        {"model_patch_applied", result.model_patch_applied},
        {"test_patch_applied", result.test_patch_applied},
        {"duration_ms", static_cast<std::int64_t>(result.duration.count())},
    };
    if (!result.error.empty()) {
        record["metadata"]["error"] = result.error;
    }
    return record;
}

}  // namespace hermes::batch
