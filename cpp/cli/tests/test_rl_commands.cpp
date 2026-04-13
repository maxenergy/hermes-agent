// Tests for the dedicated RL CLI subcommand (``hermes rl ...``).
#include "hermes/cli/rl_commands.hpp"

#include <filesystem>
#include <fstream>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

namespace hermes::cli::rl {
namespace {

class RlCommandsTest : public ::testing::Test {
protected:
    void SetUp() override {
        tmp_dir_ = std::filesystem::temp_directory_path() /
                   ("hermes_rl_test_" + std::to_string(::getpid()));
        std::filesystem::create_directories(tmp_dir_);
    }
    void TearDown() override {
        std::filesystem::remove_all(tmp_dir_);
    }
    std::filesystem::path tmp_dir_;
};

TEST_F(RlCommandsTest, DefaultPromptContainsKeySections) {
    auto s = default_rl_system_prompt();
    EXPECT_NE(s.find("reinforcement learning"), std::string::npos);
    EXPECT_NE(s.find("rl_list_environments"), std::string::npos);
    EXPECT_NE(s.find("rl_start_training"), std::string::npos);
    EXPECT_NE(s.find("30 minutes"), std::string::npos);
}

TEST_F(RlCommandsTest, TrainToolsIncludeRlToolset) {
    auto ts = rl_toolset_names(/*eval_only=*/false);
    EXPECT_NE(std::find(ts.begin(), ts.end(), "rl"), ts.end());
    EXPECT_NE(std::find(ts.begin(), ts.end(), "terminal"), ts.end());
    EXPECT_NE(std::find(ts.begin(), ts.end(), "web"), ts.end());
}

TEST_F(RlCommandsTest, EvalToolsIncludeRlToolset) {
    auto ts = rl_toolset_names(/*eval_only=*/true);
    EXPECT_NE(std::find(ts.begin(), ts.end(), "rl"), ts.end());
}

TEST_F(RlCommandsTest, LoadRlConfigFromJson) {
    auto cfg_path = tmp_dir_ / "rl.json";
    nlohmann::json j;
    j["dataset"] = "tasks.jsonl";
    j["output_dir"] = "out";
    j["model"] = "qwen/qwen3-coder";
    j["environment"] = "docker";
    j["max_iterations"] = 500;
    j["num_workers"] = 2;
    j["progress_interval"] = 15;
    j["checkpoint_interval"] = 120;
    j["eval_only"] = true;
    j["toolsets"] = {"terminal", "rl"};
    std::ofstream(cfg_path) << j.dump();

    auto cfg = load_rl_config(cfg_path);
    EXPECT_EQ(cfg.dataset_path, std::filesystem::path("tasks.jsonl"));
    EXPECT_EQ(cfg.output_dir, std::filesystem::path("out"));
    EXPECT_EQ(cfg.model, "qwen/qwen3-coder");
    EXPECT_EQ(cfg.default_environment, "docker");
    EXPECT_EQ(cfg.max_iterations, 500);
    EXPECT_EQ(cfg.num_workers, 2);
    EXPECT_EQ(cfg.progress_interval_seconds, 15);
    EXPECT_EQ(cfg.checkpoint_interval_seconds, 120);
    EXPECT_TRUE(cfg.eval_only);
    ASSERT_EQ(cfg.enabled_toolsets.size(), 2u);
    EXPECT_EQ(cfg.enabled_toolsets[0], "terminal");
}

TEST_F(RlCommandsTest, LoadRlConfigFromYaml) {
    auto cfg_path = tmp_dir_ / "rl.yaml";
    {
        std::ofstream out(cfg_path);
        out << "dataset: t.jsonl\n";
        out << "output_dir: out\n";
        out << "model: some/model\n";
        out << "max_iterations: 42\n";
    }
    auto cfg = load_rl_config(cfg_path);
    EXPECT_EQ(cfg.dataset_path, std::filesystem::path("t.jsonl"));
    EXPECT_EQ(cfg.model, "some/model");
    EXPECT_EQ(cfg.max_iterations, 42);
}

TEST_F(RlCommandsTest, LoadRlConfigMissingFileKeepsDefaults) {
    auto cfg = load_rl_config(tmp_dir_ / "does_not_exist.json");
    EXPECT_EQ(cfg.model, "anthropic/claude-opus-4.5");
    EXPECT_EQ(cfg.max_iterations, 200);
    EXPECT_FALSE(cfg.eval_only);
    EXPECT_NE(cfg.system_prompt.find("reinforcement"), std::string::npos);
}

TEST_F(RlCommandsTest, DispatchHelpReturnsZero) {
    EXPECT_EQ(dispatch_rl_command({"--help"}), 0);
    EXPECT_EQ(dispatch_rl_command({}), 0);  // empty → help
}

TEST_F(RlCommandsTest, DispatchUnknownVerbReturnsError) {
    EXPECT_EQ(dispatch_rl_command({"foo"}), 2);
}

TEST_F(RlCommandsTest, DispatchMissingValueErrors) {
    EXPECT_EQ(dispatch_rl_command({"train", "--dataset"}), 2);
}

TEST_F(RlCommandsTest, DispatchListEnvironments) {
    EXPECT_EQ(dispatch_rl_command({"list-environments"}), 0);
}

TEST_F(RlCommandsTest, RunTrainingWithDatasetWritesTrajectories) {
    // Build a 2-line JSONL dataset.
    auto ds = tmp_dir_ / "ds.jsonl";
    {
        std::ofstream out(ds);
        out << R"({"prompt":"hello"})" << "\n";
        out << "how are you?\n";
    }
    auto outdir = tmp_dir_ / "out";
    RlConfig cfg;
    cfg.dataset_path = ds;
    cfg.output_dir = outdir;
    cfg.num_workers = 1;
    cfg.progress_interval_seconds = 60;
    cfg.checkpoint_interval_seconds = 3600;
    cfg.save_trajectories = true;
    cfg.system_prompt = default_rl_system_prompt();
    EXPECT_EQ(run_rl_eval(cfg), 0);
    // Two trajectory JSON files expected.
    EXPECT_TRUE(std::filesystem::exists(outdir / "result_0.json"));
    EXPECT_TRUE(std::filesystem::exists(outdir / "result_1.json"));
    EXPECT_TRUE(std::filesystem::exists(outdir / "rl_dataset.jsonl"));
}

TEST_F(RlCommandsTest, RunWithoutDatasetReturnsError) {
    RlConfig cfg;
    cfg.output_dir = tmp_dir_ / "out";
    EXPECT_EQ(run_rl_training(cfg), 2);
}

TEST_F(RlCommandsTest, CheckpointIntervalDefaultIs30Minutes) {
    RlConfig cfg;
    EXPECT_EQ(cfg.checkpoint_interval_seconds, 30 * 60);
}

}  // namespace
}  // namespace hermes::cli::rl
