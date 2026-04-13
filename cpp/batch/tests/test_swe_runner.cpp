// Unit tests for SweRunner — uses a mock environment to avoid spawning
// real subprocesses.
#include "hermes/batch/swe_runner.hpp"

#include <filesystem>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "hermes/environments/base.hpp"

namespace hermes::batch {
namespace {

class MockEnv : public hermes::environments::BaseEnvironment {
public:
    std::string name() const override { return "mock"; }

    hermes::environments::CompletedProcess execute(
        const std::string& cmd,
        const hermes::environments::ExecuteOptions& opts) override {
        commands.push_back(cmd);
        cwds.push_back(opts.cwd.string());
        hermes::environments::CompletedProcess p;
        // Use scripted responses if provided, else default to success.
        if (!scripted.empty()) {
            p = scripted.front();
            scripted.erase(scripted.begin());
        } else {
            p.exit_code = 0;
        }
        return p;
    }

    std::vector<std::string> commands;
    std::vector<std::string> cwds;
    std::vector<hermes::environments::CompletedProcess> scripted;
};

class SweRunnerTest : public ::testing::Test {
protected:
    void SetUp() override {
        repo_ = std::filesystem::temp_directory_path() /
                ("hermes_swe_test_" + std::to_string(::getpid()));
        std::filesystem::create_directories(repo_);
    }
    void TearDown() override {
        std::filesystem::remove_all(repo_);
    }

    std::filesystem::path repo_;
};

TEST_F(SweRunnerTest, MissingRepoProducesError) {
    SweTask task;
    task.task_id = "t1";
    task.repo_path = repo_ / "does_not_exist";
    task.test_cmd = "pytest";
    MockEnv env;
    SweRunner runner(&env);
    auto r = runner.run(task);
    EXPECT_FALSE(r.tests_passed);
    EXPECT_NE(r.error.find("repo_path"), std::string::npos);
}

TEST_F(SweRunnerTest, AppliesPatchesAndRunsTests) {
    SweTask task;
    task.task_id = "t2";
    task.repo_path = repo_;
    task.test_cmd = "pytest -q";
    task.model_patch = "diff --git a/x b/x\n";
    task.test_patch = "diff --git a/y b/y\n";

    MockEnv env;
    // three successful invocations: model patch, test patch, pytest
    for (int i = 0; i < 3; ++i) {
        hermes::environments::CompletedProcess p;
        p.exit_code = 0;
        env.scripted.push_back(p);
    }
    SweRunner runner(&env);
    auto r = runner.run(task);
    EXPECT_TRUE(r.model_patch_applied);
    EXPECT_TRUE(r.test_patch_applied);
    EXPECT_TRUE(r.tests_passed);
    EXPECT_EQ(r.test_exit_code, 0);
    ASSERT_EQ(env.commands.size(), 3u);
    EXPECT_NE(env.commands[0].find("git apply"), std::string::npos);
    EXPECT_NE(env.commands[1].find("git apply"), std::string::npos);
    EXPECT_EQ(env.commands[2], "pytest -q");
}

TEST_F(SweRunnerTest, ModelPatchFailureShortCircuits) {
    SweTask task;
    task.task_id = "t3";
    task.repo_path = repo_;
    task.model_patch = "diff --git a/bad b/bad\n";

    MockEnv env;
    hermes::environments::CompletedProcess bad;
    bad.exit_code = 1;
    bad.stderr_text = "reject";
    env.scripted.push_back(bad);

    SweRunner runner(&env);
    auto r = runner.run(task);
    EXPECT_FALSE(r.model_patch_applied);
    EXPECT_FALSE(r.tests_passed);
    EXPECT_NE(r.error.find("model_patch"), std::string::npos);
    // Only the model_patch command was issued — no test execution.
    EXPECT_EQ(env.commands.size(), 1u);
}

TEST_F(SweRunnerTest, TestFailureRecordsExitCodeAndOutput) {
    SweTask task;
    task.task_id = "t4";
    task.repo_path = repo_;
    task.test_cmd = "pytest";

    MockEnv env;
    hermes::environments::CompletedProcess bad_test;
    bad_test.exit_code = 1;
    bad_test.stdout_text = "1 failed";
    bad_test.stderr_text = "assert err";
    env.scripted.push_back(bad_test);
    SweRunner runner(&env);
    auto r = runner.run(task);
    EXPECT_FALSE(r.tests_passed);
    EXPECT_EQ(r.test_exit_code, 1);
    EXPECT_EQ(r.test_stdout, "1 failed");
    EXPECT_EQ(r.test_stderr, "assert err");
}

TEST_F(SweRunnerTest, HfRecordShape) {
    SweTask task;
    task.task_id = "t5";
    task.problem_statement = "Fix the bug.";
    task.model_patch = "diff --git a/x b/x\n";
    SweResult result;
    result.task_id = "t5";
    result.tests_passed = true;

    MockEnv env;
    SweRunner runner(&env);
    auto rec = runner.to_hf_record(task, result);
    EXPECT_TRUE(rec.contains("conversations"));
    ASSERT_EQ(rec["conversations"].size(), 4u);
    EXPECT_EQ(rec["conversations"][0]["from"], "system");
    EXPECT_EQ(rec["conversations"][1]["from"], "human");
    EXPECT_EQ(rec["conversations"][1]["value"], "Fix the bug.");
    EXPECT_EQ(rec["conversations"][2]["from"], "gpt");
    EXPECT_EQ(rec["conversations"][3]["from"], "tool");
    EXPECT_EQ(rec["metadata"]["task_id"], "t5");
    EXPECT_EQ(rec["metadata"]["tests_passed"], true);
}

}  // namespace
}  // namespace hermes::batch
