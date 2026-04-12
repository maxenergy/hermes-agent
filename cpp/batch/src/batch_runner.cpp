#include "hermes/batch/batch_runner.hpp"

#include <fstream>
#include <mutex>
#include <thread>

#include <nlohmann/json.hpp>

namespace hermes::batch {

BatchRunner::BatchRunner(BatchConfig config) : config_(std::move(config)) {}

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

    auto worker = [&]() {
        while (true) {
            int idx;
            {
                std::lock_guard<std::mutex> lock(mu);
                if (next_index >= static_cast<int>(prompts.size())) {
                    return;
                }
                idx = next_index++;
            }

            try {
                auto prompt_json = nlohmann::json::parse(prompts[static_cast<size_t>(idx)]);

                // Write result
                if (config_.save_trajectories) {
                    auto out_path = config_.output_dir /
                                    ("result_" + std::to_string(idx) + ".json");
                    std::ofstream out(out_path);
                    out << prompt_json.dump(2);
                }

                {
                    std::lock_guard<std::mutex> lock(mu);
                    result.completed++;
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

    auto end = std::chrono::steady_clock::now();
    result.duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);

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
            auto prompt_json = nlohmann::json::parse(prompts[i]);
            if (config_.save_trajectories) {
                auto out_path = config_.output_dir /
                                ("result_" + std::to_string(i) + ".json");
                std::ofstream out(out_path);
                out << prompt_json.dump(2);
            }
            result.completed++;
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
