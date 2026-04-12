// BatchRunner — runs a JSONL dataset through AIAgent with a thread pool.
#pragma once

#include <chrono>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace hermes::batch {

struct BatchConfig {
    std::filesystem::path dataset_path;  // JSONL input
    std::filesystem::path output_dir;
    int num_workers = 4;
    std::string model;
    std::vector<std::string> enabled_toolsets;
    std::string distribution = "default";  // toolset distribution name
    bool save_trajectories = true;
};

struct BatchResult {
    int total_prompts = 0;
    int completed = 0;
    int failed = 0;
    std::map<std::string, int> tool_stats;  // tool_name -> call count
    std::chrono::milliseconds duration{0};
};

class BatchRunner {
public:
    explicit BatchRunner(BatchConfig config);

    // Run all prompts from dataset. Uses thread pool.
    BatchResult run();

    // Resume from checkpoint
    BatchResult resume();

private:
    BatchConfig config_;
    // Checkpoint: {output_dir}/checkpoint.json tracks completed prompt indices
    void save_checkpoint(int completed_index);
    int load_checkpoint();
};

}  // namespace hermes::batch
