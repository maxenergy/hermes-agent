#include "hermes/agent/subdirectory_hints.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>

using hermes::agent::SubdirectoryHintTracker;
using hermes::agent::SubdirectoryHintDiscoverer;
namespace fs = std::filesystem;

// ── Legacy LRU tracker tests ─────────────────────────────────────────

TEST(SubdirectoryHintTracker, RecordsAndReturnsLruOrder) {
    SubdirectoryHintTracker t(8);
    t.record_edit(fs::path("/proj/src/foo.cpp"));
    t.record_edit(fs::path("/proj/include/bar.hpp"));
    t.record_edit(fs::path("/proj/tests/baz.cpp"));

    auto recent = t.recent(2);
    ASSERT_EQ(recent.size(), 2u);
    EXPECT_EQ(recent[0], "/proj/tests");
    EXPECT_EQ(recent[1], "/proj/include");
}

TEST(SubdirectoryHintTracker, RepeatedEditMovesToFront) {
    SubdirectoryHintTracker t(8);
    t.record_edit(fs::path("/a/x.cpp"));
    t.record_edit(fs::path("/b/y.cpp"));
    t.record_edit(fs::path("/c/z.cpp"));
    t.record_edit(fs::path("/a/x2.cpp"));

    auto recent = t.recent(3);
    ASSERT_EQ(recent.size(), 3u);
    EXPECT_EQ(recent[0], "/a");
    EXPECT_EQ(recent[1], "/c");
    EXPECT_EQ(recent[2], "/b");
}

TEST(SubdirectoryHintTracker, CapacityEvictsOldest) {
    SubdirectoryHintTracker t(2);
    t.record_edit(fs::path("/d1/x"));
    t.record_edit(fs::path("/d2/x"));
    t.record_edit(fs::path("/d3/x"));
    EXPECT_EQ(t.size(), 2u);
    auto r = t.recent(5);
    ASSERT_EQ(r.size(), 2u);
    EXPECT_EQ(r[0], "/d3");
    EXPECT_EQ(r[1], "/d2");
}

TEST(SubdirectoryHintTracker, ClearEmptiesEverything) {
    SubdirectoryHintTracker t;
    t.record_edit(fs::path("/foo/bar.txt"));
    t.clear();
    EXPECT_EQ(t.size(), 0u);
    EXPECT_TRUE(t.recent(5).empty());
}

// ── Discoverer tests ────────────────────────────────────────────────

namespace {

class DiscovererFixture : public ::testing::Test {
protected:
    fs::path root_;

    void SetUp() override {
        root_ = fs::temp_directory_path() /
                ("hermes_subdir_hints_" + std::to_string(::getpid()) + "_" +
                 std::to_string(reinterpret_cast<uintptr_t>(this)));
        fs::create_directories(root_);
        fs::create_directories(root_ / "backend" / "src");
        fs::create_directories(root_ / "frontend");
        // Write an AGENTS.md into backend/ only.
        std::ofstream(root_ / "backend" / "AGENTS.md")
            << "# Backend notes\nUse Rust.";
        std::ofstream(root_ / "frontend" / "CLAUDE.md")
            << "# Frontend notes\nUse TSX.";
    }

    void TearDown() override {
        std::error_code ec;
        fs::remove_all(root_, ec);
    }
};

}  // namespace

TEST_F(DiscovererFixture, DiscoversAgentsMdOnReadFile) {
    SubdirectoryHintDiscoverer d(root_);
    nlohmann::json args = {{"path", "backend/src/main.rs"}};
    auto hint = d.check_tool_call("read_file", args);
    ASSERT_TRUE(hint.has_value());
    EXPECT_NE(hint->find("Backend notes"), std::string::npos);
    EXPECT_NE(hint->find("Subdirectory context discovered"), std::string::npos);
}

TEST_F(DiscovererFixture, SecondCallDoesNotDuplicate) {
    SubdirectoryHintDiscoverer d(root_);
    nlohmann::json args = {{"path", "backend/src/main.rs"}};
    auto first = d.check_tool_call("read_file", args);
    ASSERT_TRUE(first.has_value());
    auto second = d.check_tool_call("read_file", args);
    EXPECT_FALSE(second.has_value());
}

TEST_F(DiscovererFixture, TerminalCommandExtractsPath) {
    SubdirectoryHintDiscoverer d(root_);
    nlohmann::json args = {
        {"command", "cat " + (root_ / "frontend" / "x.tsx").string()},
    };
    auto hint = d.check_tool_call("terminal", args);
    ASSERT_TRUE(hint.has_value());
    EXPECT_NE(hint->find("Frontend notes"), std::string::npos);
}

TEST_F(DiscovererFixture, NoHintsInDirReturnsNullopt) {
    // Create a directory without any hint files.
    fs::create_directories(root_ / "empty");
    SubdirectoryHintDiscoverer d(root_);
    nlohmann::json args = {{"path", "empty/file.txt"}};
    auto hint = d.check_tool_call("read_file", args);
    EXPECT_FALSE(hint.has_value());
}

TEST_F(DiscovererFixture, WorkingDirPrefilled) {
    SubdirectoryHintDiscoverer d(root_);
    EXPECT_GT(d.loaded_dirs_for_testing().size(), 0u);
}

TEST(SubdirHintsDetail, ShellTokeniseHandlesQuotes) {
    auto toks = hermes::agent::subdir_hints_detail::shell_tokenise(
        R"(cat "path with spaces/file.txt" other)");
    ASSERT_EQ(toks.size(), 3u);
    EXPECT_EQ(toks[0], "cat");
    EXPECT_EQ(toks[1], "path with spaces/file.txt");
    EXPECT_EQ(toks[2], "other");
}

TEST(SubdirHintsDetail, LooksLikePathTokenFiltersFlagsAndUrls) {
    using hermes::agent::subdir_hints_detail::looks_like_path_token;
    EXPECT_TRUE(looks_like_path_token("src/main.cpp"));
    EXPECT_TRUE(looks_like_path_token("./foo"));
    EXPECT_FALSE(looks_like_path_token("--flag"));
    EXPECT_FALSE(looks_like_path_token("-v"));
    EXPECT_FALSE(looks_like_path_token("https://example.com/path"));
    EXPECT_FALSE(looks_like_path_token("git@github.com:x/y"));
    EXPECT_FALSE(looks_like_path_token("plainword"));
}
