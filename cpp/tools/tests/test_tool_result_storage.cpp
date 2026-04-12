#include "hermes/tools/tool_result_storage.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <thread>

using hermes::tools::ToolResultStorage;

namespace {

class ToolResultStorageTest : public ::testing::Test {
protected:
    std::filesystem::path tmp_dir;

    void SetUp() override {
        tmp_dir = std::filesystem::temp_directory_path() /
                  ("hermes_test_trs_" + std::to_string(
                      std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::create_directories(tmp_dir);
    }

    void TearDown() override {
        std::error_code ec;
        std::filesystem::remove_all(tmp_dir, ec);
    }
};

}  // namespace

TEST_F(ToolResultStorageTest, StoreRetrieveRoundTrip) {
    ToolResultStorage storage(tmp_dir);
    std::string content = "This is a large tool result payload.";
    auto handle = storage.store(content);
    EXPECT_FALSE(handle.empty());
    EXPECT_EQ(handle.substr(0, 21), "hermes://tool-result/");

    auto retrieved = storage.retrieve(handle);
    ASSERT_TRUE(retrieved.has_value());
    EXPECT_EQ(*retrieved, content);
}

TEST_F(ToolResultStorageTest, RetrieveUnknownReturnsNullopt) {
    ToolResultStorage storage(tmp_dir);
    auto result = storage.retrieve("hermes://tool-result/nonexistent-uuid");
    EXPECT_FALSE(result.has_value());
}

TEST_F(ToolResultStorageTest, CleanupRemovesOld) {
    ToolResultStorage storage(tmp_dir);
    storage.store("old content");

    // Cleanup with 0 seconds age should remove everything.
    auto removed = storage.cleanup_older_than(std::chrono::seconds(0));
    EXPECT_GE(removed, 1u);
}
