#include "hermes/state/checkpoint_manager.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <thread>

namespace {

namespace fs = std::filesystem;
using hermes::state::CheckpointManager;

class CheckpointManagerTest : public ::testing::Test {
protected:
    fs::path root;
    fs::path workspace;

    void SetUp() override {
        root = fs::temp_directory_path() / ("hermes_cp_test_" +
                                            std::to_string(::getpid()) + "_" +
                                            std::to_string(reinterpret_cast<uintptr_t>(this)));
        workspace = root / "ws";
        fs::remove_all(root);
        fs::create_directories(workspace);
        std::ofstream(workspace / "a.txt") << "alpha";
        fs::create_directories(workspace / "sub");
        std::ofstream(workspace / "sub" / "b.txt") << "beta";
    }

    void TearDown() override { fs::remove_all(root); }
};

TEST_F(CheckpointManagerTest, CreateAndGetRoundTrip) {
    CheckpointManager mgr(root / "cps");
    auto cp = mgr.create("task1", "label1", workspace,
                         nlohmann::json{{"model", "gpt-5"}});
    EXPECT_EQ(cp.task_id, "task1");
    EXPECT_EQ(cp.label, "label1");
    EXPECT_TRUE(fs::exists(cp.snapshot_dir / "a.txt"));
    EXPECT_TRUE(fs::exists(cp.snapshot_dir / "sub" / "b.txt"));

    auto loaded = mgr.get("task1", "label1");
    ASSERT_TRUE(loaded.has_value());
    EXPECT_EQ(loaded->metadata["model"], "gpt-5");
}

TEST_F(CheckpointManagerTest, RestoreOverwritesWorkspace) {
    CheckpointManager mgr(root / "cps");
    mgr.create("t", "v1", workspace);

    // Modify workspace.
    std::ofstream(workspace / "a.txt") << "modified";

    // Restore without overwrite should fail.
    EXPECT_FALSE(mgr.restore("t", "v1", workspace, /*overwrite=*/false));
    // With overwrite it succeeds.
    EXPECT_TRUE(mgr.restore("t", "v1", workspace, /*overwrite=*/true));
    std::ifstream ifs(workspace / "a.txt");
    std::string content((std::istreambuf_iterator<char>(ifs)), {});
    EXPECT_EQ(content, "alpha");
}

TEST_F(CheckpointManagerTest, ListAndRemove) {
    CheckpointManager mgr(root / "cps");
    mgr.create("t", "first", workspace);
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    mgr.create("t", "second", workspace);
    auto cps = mgr.list("t");
    EXPECT_EQ(cps.size(), 2u);
    // Newest first.
    EXPECT_EQ(cps[0].label, "second");

    mgr.remove("t", "first");
    cps = mgr.list("t");
    EXPECT_EQ(cps.size(), 1u);
    EXPECT_EQ(cps[0].label, "second");
}

TEST_F(CheckpointManagerTest, GetReturnsNulloptWhenMissing) {
    CheckpointManager mgr(root / "cps");
    EXPECT_FALSE(mgr.get("none", "none").has_value());
}

}  // namespace
