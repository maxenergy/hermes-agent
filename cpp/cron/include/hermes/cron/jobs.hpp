// Job definitions and JSON-file persistence for cron jobs.
#pragma once

#include <hermes/cron/cron_parser.hpp>

#include <chrono>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace hermes::cron {

struct Job {
    std::string id;
    std::string name;
    CronExpression schedule;
    std::string schedule_str;  // original cron string
    std::string prompt;        // agent prompt to execute
    std::string model;         // optional model override
    std::vector<std::string> delivery_targets;  // "telegram", "local", etc.
    bool paused = false;
    int max_retries = 3;
    std::chrono::system_clock::time_point created_at;
    std::chrono::system_clock::time_point last_run;
    std::chrono::system_clock::time_point next_run;
    int run_count = 0;
    int fail_count = 0;
};

struct JobResult {
    std::string job_id;
    std::string run_id;
    std::string output;
    bool success;
    std::chrono::system_clock::time_point started_at;
    std::chrono::system_clock::time_point finished_at;
};

class JobStore {
public:
    explicit JobStore(std::filesystem::path store_dir);

    std::string create(Job job);
    std::optional<Job> get(const std::string& id);
    std::vector<Job> list_all();
    void update(const Job& job);
    void remove(const std::string& id);

    void save_result(const JobResult& result);
    std::vector<JobResult> get_results(const std::string& job_id,
                                       int limit = 10);

private:
    std::filesystem::path dir_;
    // Jobs stored as individual JSON files: {dir}/jobs/{id}.json
    // Results stored as JSONL: {dir}/results/{job_id}.jsonl

    std::filesystem::path job_path(const std::string& id) const;
    std::filesystem::path result_path(const std::string& job_id) const;
};

}  // namespace hermes::cron
