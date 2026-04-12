#include "hermes/tools/file_operations.hpp"
#include "hermes/tools/file_tools.hpp"
#include "hermes/tools/registry.hpp"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;
using json = nlohmann::json;
using namespace hermes::tools;

namespace {

class FileToolsTest : public ::testing::Test {
protected:
    void SetUp() override {
        ToolRegistry::instance().clear();
        register_file_tools();

        // Create temp directory.
        tmp_ = fs::temp_directory_path() / "hermes_file_tools_test";
        fs::create_directories(tmp_);
    }

    void TearDown() override {
        ToolRegistry::instance().clear();
        std::error_code ec;
        fs::remove_all(tmp_, ec);
    }

    std::string dispatch(const std::string& name, const json& args) {
        ToolContext ctx;
        ctx.cwd = tmp_.string();
        return ToolRegistry::instance().dispatch(name, args, ctx);
    }

    void write_tmp(const std::string& name, const std::string& content) {
        std::ofstream out(tmp_ / name);
        out << content;
    }

    fs::path tmp_;
};

// -- read_file tests -------------------------------------------------------

TEST_F(FileToolsTest, ReadFileHappyPath) {
    write_tmp("hello.txt", "Hello, World!");
    auto result = json::parse(dispatch("read_file", {{"path", "hello.txt"}}));
    EXPECT_EQ(result["content"], "Hello, World!");
}

TEST_F(FileToolsTest, ReadFileWithOffsetAndLimit) {
    write_tmp("data.txt", "abcdefghij");
    auto result = json::parse(
        dispatch("read_file", {{"path", "data.txt"}, {"offset", 3}, {"limit", 4}}));
    EXPECT_EQ(result["content"], "defg");
}

TEST_F(FileToolsTest, ReadFileBlocksDevPaths) {
    auto result = json::parse(dispatch("read_file", {{"path", "/dev/zero"}}));
    EXPECT_TRUE(result.contains("error"));
    EXPECT_NE(result["error"].get<std::string>().find("/dev/"), std::string::npos);
}

// -- write_file tests ------------------------------------------------------

TEST_F(FileToolsTest, WriteFileRoundTrip) {
    auto wr = json::parse(
        dispatch("write_file", {{"path", "out.txt"}, {"content", "test data"}}));
    EXPECT_TRUE(wr["written"].get<bool>());
    EXPECT_EQ(wr["bytes"], 9);

    auto rd = json::parse(dispatch("read_file", {{"path", "out.txt"}}));
    EXPECT_EQ(rd["content"], "test data");
}

TEST_F(FileToolsTest, WriteFileBlocksSensitivePaths) {
    auto result = json::parse(
        dispatch("write_file", {{"path", "/etc/passwd"}, {"content", "x"}}));
    EXPECT_TRUE(result.contains("error"));
}

// -- patch tests -----------------------------------------------------------

TEST_F(FileToolsTest, PatchAppliesValidDiff) {
    write_tmp("patch_target.txt", "line1\nline2\nline3\n");
    std::string diff =
        "--- a/patch_target.txt\n"
        "+++ b/patch_target.txt\n"
        "@@ -1,3 +1,3 @@\n"
        " line1\n"
        "-line2\n"
        "+line2_modified\n"
        " line3\n";

    auto result = json::parse(
        dispatch("patch", {{"path", "patch_target.txt"}, {"diff", diff}}));
    EXPECT_TRUE(result["patched"].get<bool>());
    EXPECT_EQ(result["hunks_applied"], 1);

    // Verify content changed.
    auto rd = json::parse(dispatch("read_file", {{"path", "patch_target.txt"}}));
    EXPECT_NE(rd["content"].get<std::string>().find("line2_modified"),
              std::string::npos);
}

TEST_F(FileToolsTest, PatchFailsOnMismatchedContext) {
    write_tmp("mismatch.txt", "aaa\nbbb\nccc\n");
    std::string diff =
        "--- a/mismatch.txt\n"
        "+++ b/mismatch.txt\n"
        "@@ -1,3 +1,3 @@\n"
        " xxx\n"
        "-bbb\n"
        "+yyy\n"
        " ccc\n";

    auto result = json::parse(
        dispatch("patch", {{"path", "mismatch.txt"}, {"diff", diff}}));
    EXPECT_TRUE(result.contains("error"));
    EXPECT_NE(result["error"].get<std::string>().find("context mismatch"),
              std::string::npos);
}

// -- search_files tests ----------------------------------------------------

TEST_F(FileToolsTest, SearchFilesFindsPattern) {
    fs::create_directories(tmp_ / "sub");
    write_tmp("sub/a.cpp", "int main() { return 0; }");
    write_tmp("sub/b.txt", "no match here");
    write_tmp("sub/c.cpp", "// main function\nint main() {}");

    auto result = json::parse(
        dispatch("search_files",
                 {{"pattern", "main"}, {"path", "sub"}, {"glob", "*.cpp"}}));
    ASSERT_TRUE(result.contains("matches"));
    EXPECT_GE(result["matches"].size(), 2u);

    // Verify match structure.
    for (const auto& m : result["matches"]) {
        EXPECT_TRUE(m.contains("file"));
        EXPECT_TRUE(m.contains("line"));
        EXPECT_TRUE(m.contains("text"));
    }
}

}  // namespace
