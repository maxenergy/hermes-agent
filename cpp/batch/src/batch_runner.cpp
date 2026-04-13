#include "hermes/batch/batch_runner.hpp"

#include <atomic>
#include <condition_variable>
#include <fstream>
#include <iostream>
#include <mutex>
#include <thread>

#include <nlohmann/json.hpp>

#include "hermes/batch/hf_schema.hpp"
#include "hermes/batch/swe_runner.hpp"
#include "hermes/environments/local.hpp"

namespace hermes::batch {

nlohmann::json BatchProgress::to_json() const {
    return {
        {"total", total},
        {"completed", completed},
        {"failed", failed},
        {"elapsed_ms", static_cast<std::int64_t>(elapsed.count())},
        {"is_final", is_final},
    };
}

BatchRunner::BatchRunner(BatchConfig config) : config_(std::move(config)) {}

std::string BatchRunner::resolve_env(const nlohmann::json& task) const {
    if (task.is_object() && task.contains("environment") &&
        task["environment"].is_string()) {
        return task["environment"].get<std::string>();
    }
    return config_.default_environment;
}

std::string BatchRunner::resolve_type(const nlohmann::json& task) const {
    if (task.is_object() && task.contains("type") && task["type"].is_string()) {
        return task["type"].get<std::string>();
    }
    return config_.default_task_type;
}

void BatchRunner::emit_progress(const BatchProgress& p) const {
    if (progress_cb_) {
        progress_cb_(p);
    } else {
        std::cerr << "hermes-batch progress: " << p.to_json().dump()
                  << std::endl;
    }
}

namespace {

// Factory: create a fresh environment instance for a task.  Docker /
// Modal / Singularity adapters are selected by name; unrecognised names
// fall back to ``local``.  Keeping the factory local avoids a hard link
// dependency on every env adapter — callers that want remote execution
// pre-link the relevant static archives.
std::unique_ptr<hermes::environments::BaseEnvironment> make_env(
    const std::string& name) {
    // NB: this port keeps the default ``local`` backend only.  Docker /
    // Modal / Managed-Modal adapters exist in
    // ``cpp/environments`` and can be plugged in here once the
    // corresponding link dependencies are wired at the task-runner
    // level.  For now all non-local names fall back to local + emit a
    // warning on stderr so SWE tasks still execute.
    if (name != "local") {
        std::cerr << "hermes-batch: environment '" << name
                  << "' not wired in C++ runner yet, using local.\n";
    }
    return std::make_unique<hermes::environments::LocalEnvironment>();
}

// Parse a JSONL line into either a plain prompt string or a SWE task
// description.  Used by both ``run`` and ``resume``.
nlohmann::json parse_task_line(const std::string& line) {
    try {
        return nlohmann::json::parse(line);
    } catch (...) {
        return nlohmann::json();
    }
}

void write_trajectory_record(const std::filesystem::path& output_dir,
                              int idx,
                              const nlohmann::json& record) {
    auto out_path = output_dir / ("result_" + std::to_string(idx) + ".json");
    std::ofstream out(out_path);
    out << record.dump(2);
}

// Dispatch a single task to the right executor.  Returns a pair
// {record, passed} where ``passed`` only meaningful for SWE tasks.
std::pair<nlohmann::json, int> execute_task(const BatchRunner& runner,
                                              const nlohmann::json& task,
                                              const BatchConfig& config) {
    const auto type = runner.resolve_type(task);
    const auto env_name = runner.resolve_env(task);

    if (type == "swe") {
        SweTask swe;
        swe.task_id = task.value("task_id", std::string("unknown"));
        swe.problem_statement = task.value("problem_statement", std::string{});
        swe.repo_path = std::filesystem::path(
            task.value("repo_path", std::string{}));
        swe.test_patch = task.value("test_patch", std::string{});
        swe.model_patch = task.value("model_patch", std::string{});
        swe.test_cmd = task.value("test_cmd", std::string("pytest -x"));
        if (task.contains("timeout_seconds") &&
            task["timeout_seconds"].is_number_integer()) {
            swe.timeout =
                std::chrono::seconds(task["timeout_seconds"].get<int>());
        }
        auto env = make_env(env_name);
        SweRunner sr(env.get());
        auto result = sr.run(swe);
        auto record = sr.to_hf_record(swe, result);
        record["environment"] = env_name;
        return {record, result.tests_passed ? 1 : 0};
    }

    // Prompt-style task: carry over the raw task JSON and record the
    // environment it was assigned to.  When an AIAgent adapter is wired
    // in this is where we would call it; for now we persist the prompt
    // verbatim so downstream SFT tooling can process the JSONL.
    (void)config;
    nlohmann::json record;
    record["conversations"] = nlohmann::json::array();
    if (task.contains("prompt") && task["prompt"].is_string()) {
        record["conversations"].push_back({
            {"from", "human"}, {"value", task["prompt"].get<std::string>()},
        });
    }
    record["metadata"] = task;
    record["environment"] = env_name;
    return {record, -1};
}

}  // namespace

BatchResult BatchRunner::run() {
    auto start = std::chrono::steady_clock::now();

    BatchResult result;

    // Read JSONL dataset
    std::vector<std::string> prompts;
    {
        std::ifstream in(config_.dataset_path);
        std::string line;
        while (std::getline(in, line)) {
            if (!line.empty()) {
                prompts.push_back(line);
            }
        }
    }
    result.total_prompts = static_cast<int>(prompts.size());

    // Ensure output directory exists
    std::filesystem::create_directories(config_.output_dir);

    // Process prompts with thread pool
    std::mutex mu;
    int next_index = 0;
    int num_workers = std::max(1, config_.num_workers);

    // Progress / checkpoint watchdog
    std::atomic<bool> done{false};
    std::mutex watchdog_mu;
    std::condition_variable watchdog_cv;

    auto watchdog = [&]() {
        auto last_checkpoint = std::chrono::steady_clock::now();
        while (true) {
            std::unique_lock<std::mutex> lk(watchdog_mu);
            if (watchdog_cv.wait_for(lk, config_.progress_interval,
                                     [&] { return done.load(); })) {
                return;
            }
            BatchProgress p;
            {
                std::lock_guard<std::mutex> lock(mu);
                p.total = result.total_prompts;
                p.completed = result.completed;
                p.failed = result.failed;
            }
            auto now = std::chrono::steady_clock::now();
            p.elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                now - start);
            emit_progress(p);

            if (now - last_checkpoint >= config_.checkpoint_interval) {
                // 30-min soft checkpoint: persist a snapshot of progress
                // alongside the regular completed-index checkpoint.
                auto cp_path = config_.output_dir / "progress.json";
                std::ofstream out(cp_path);
                out << p.to_json().dump(2);
                last_checkpoint = now;
            }
        }
    };
    std::thread watchdog_thread(watchdog);

    auto worker = [&]() {
        while (true) {
            int idx;
            std::string line;
            {
                std::lock_guard<std::mutex> lock(mu);
                if (next_index >= static_cast<int>(prompts.size())) {
                    return;
                }
                idx = next_index++;
                line = prompts[static_cast<size_t>(idx)];
            }

            try {
                auto task_json = parse_task_line(line);

                auto [record, swe_passed] = execute_task(*this, task_json,
                                                          config_);

                if (config_.save_trajectories) {
                    write_trajectory_record(config_.output_dir, idx, record);
                }

                {
                    std::lock_guard<std::mutex> lock(mu);
                    result.completed++;
                    if (swe_passed == 1) {
                        result.swe_passed++;
                    } else if (swe_passed == 0) {
                        result.swe_failed++;
                    }
                    save_checkpoint(idx);
                }
            } catch (...) {
                std::lock_guard<std::mutex> lock(mu);
                result.failed++;
            }
        }
    };

    std::vector<std::thread> threads;
    for (int i = 0; i < num_workers; ++i) {
        threads.emplace_back(worker);
    }
    for (auto& t : threads) {
        t.join();
    }

    // Shut down watchdog.
    {
        std::lock_guard<std::mutex> lk(watchdog_mu);
        done = true;
    }
    watchdog_cv.notify_all();
    watchdog_thread.join();

    auto end = std::chrono::steady_clock::now();
    result.duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);

    // Final progress event
    BatchProgress final_p;
    final_p.total = result.total_prompts;
    final_p.completed = result.completed;
    final_p.failed = result.failed;
    final_p.elapsed = result.duration;
    final_p.is_final = true;
    emit_progress(final_p);

    return result;
}

BatchResult BatchRunner::resume() {
    int checkpoint = load_checkpoint();

    // Re-read dataset and skip completed
    std::vector<std::string> prompts;
    {
        std::ifstream in(config_.dataset_path);
        std::string line;
        while (std::getline(in, line)) {
            if (!line.empty()) {
                prompts.push_back(line);
            }
        }
    }

    BatchResult result;
    result.total_prompts = static_cast<int>(prompts.size());
    result.completed = checkpoint + 1;  // already completed up to checkpoint

    // Process remaining
    auto start = std::chrono::steady_clock::now();
    for (size_t i = static_cast<size_t>(checkpoint + 1); i < prompts.size(); ++i) {
        try {
            auto task_json = parse_task_line(prompts[i]);
            auto [record, swe_passed] = execute_task(*this, task_json, config_);
            if (config_.save_trajectories) {
                write_trajectory_record(config_.output_dir,
                                         static_cast<int>(i), record);
            }
            result.completed++;
            if (swe_passed == 1) result.swe_passed++;
            else if (swe_passed == 0) result.swe_failed++;
            save_checkpoint(static_cast<int>(i));
        } catch (...) {
            result.failed++;
        }
    }
    auto end = std::chrono::steady_clock::now();
    result.duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);

    return result;
}

void BatchRunner::save_checkpoint(int completed_index) {
    auto cp_path = config_.output_dir / "checkpoint.json";
    nlohmann::json cp;
    cp["completed_index"] = completed_index;
    std::ofstream out(cp_path);
    out << cp.dump();
}

int BatchRunner::load_checkpoint() {
    auto cp_path = config_.output_dir / "checkpoint.json";
    if (!std::filesystem::exists(cp_path)) {
        return -1;
    }
    std::ifstream in(cp_path);
    auto cp = nlohmann::json::parse(in);
    return cp.value("completed_index", -1);
}

}  // namespace hermes::batch
