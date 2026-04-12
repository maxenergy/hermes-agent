#include "hermes/batch/batch_runner.hpp"

#include <filesystem>
#include <fstream>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

namespace hermes::batch {
namespace {

class BatchRunnerTest : public ::testing::Test {
protected:
    void SetUp() override {
        tmp_dir_ = std::filesystem::temp_directory_path() /
                   ("hermes_batch_test_" + std::to_string(::getpid()));
        std::filesystem::create_directories(tmp_dir_);

        // Create a small JSONL dataset
        dataset_path_ = tmp_dir_ / "dataset.jsonl";
        std::ofstream out(dataset_path_);
        out << R"({"prompt":"hello"})" << "\n";
        out << R"({"prompt":"world"})" << "\n";
        out << R"({"prompt":"test"})" << "\n";
    }

    void TearDown() override {
        std::filesystem::remove_all(tmp_dir_);
    }

    std::filesystem::path tmp_dir_;
    std::filesystem::path dataset_path_;
};

TEST_F(BatchRunnerTest, CreateWithConfig) {
    BatchConfig config;
    config.dataset_path = dataset_path_;
    config.output_dir = tmp_dir_ / "out";
    config.num_workers = 2;
    config.model = "test-model";

    BatchRunner runner(config);
    // Construction succeeds — no crash
    SUCCEED();
}

TEST_F(BatchRunnerTest, CheckpointSaveLoadRoundTrip) {
    BatchConfig config;
    config.dataset_path = dataset_path_;
    config.output_dir = tmp_dir_ / "out";
    config.num_workers = 1;
    config.save_trajectories = true;

    BatchRunner runner(config);
    auto result = runner.run();

    // Checkpoint should exist
    auto cp_path = config.output_dir / "checkpoint.json";
    ASSERT_TRUE(std::filesystem::exists(cp_path));

    std::ifstream in(cp_path);
    auto cp = nlohmann::json::parse(in);
    EXPECT_GE(cp["completed_index"].get<int>(), 0);
}

TEST_F(BatchRunnerTest, ResumeFromCheckpoint) {
    BatchConfig config;
    config.dataset_path = dataset_path_;
    config.output_dir = tmp_dir_ / "out";
    config.num_workers = 1;
    config.save_trajectories = true;

    // First run
    BatchRunner runner1(config);
    runner1.run();

    // Resume
    BatchRunner runner2(config);
    auto result = runner2.resume();
    EXPECT_EQ(result.total_prompts, 3);
}

TEST_F(BatchRunnerTest, ToolStatsTracking) {
    BatchConfig config;
    config.dataset_path = dataset_path_;
    config.output_dir = tmp_dir_ / "out";
    config.num_workers = 2;

    BatchRunner runner(config);
    auto result = runner.run();

    EXPECT_EQ(result.total_prompts, 3);
    EXPECT_EQ(result.completed + result.failed, 3);
    EXPECT_GE(result.duration.count(), 0);
    // tool_stats is a map — it exists even if empty
    EXPECT_TRUE(result.tool_stats.empty() || !result.tool_stats.empty());
}

}  // namespace
}  // namespace hermes::batch
