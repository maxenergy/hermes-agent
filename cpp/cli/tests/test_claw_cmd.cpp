// Tests for hermes::cli::claw_cmd (status / cleanup scanning).
#include "hermes/cli/claw_cmd.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>

using namespace hermes::cli::claw_cmd;
namespace fs = std::filesystem;

namespace {

class TempDir {
public:
    TempDir() {
        path_ = fs::temp_directory_path() / ("claw_cmd_test_" +
                std::to_string(rand()));
        fs::create_directories(path_);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path_, ec);
    }
    const fs::path& path() const { return path_; }

private:
    fs::path path_;
};

void touch(const fs::path& p, const std::string& body = "x") {
    fs::create_directories(p.parent_path());
    std::ofstream f(p);
    f << body;
}

}  // namespace

TEST(ClawCmd, ScanEmpty_ReportsMissing) {
    TempDir d;
    auto scan = scan_openclaw(d.path() / "nope");
    EXPECT_FALSE(scan.exists);
    ASSERT_FALSE(scan.findings.empty());
}

TEST(ClawCmd, ScanDetectsSoulAndMemory) {
    TempDir d;
    touch(d.path() / "SOUL.md");
    touch(d.path() / "MEMORY.md");
    auto scan = scan_openclaw(d.path());
    EXPECT_TRUE(scan.exists);
    EXPECT_TRUE(scan.has_soul);
    EXPECT_TRUE(scan.has_memory);
    EXPECT_FALSE(scan.has_user);
}

TEST(ClawCmd, ScanCountsSkills) {
    TempDir d;
    touch(d.path() / "skills" / "a.md");
    touch(d.path() / "skills" / "b.md");
    touch(d.path() / "skills" / "c.md");
    auto scan = scan_openclaw(d.path());
    EXPECT_TRUE(scan.has_skills);
    EXPECT_EQ(scan.skill_count, 3);
}

TEST(ClawCmd, ArchiveDirectory_DryRunLeavesSource) {
    TempDir parent;
    fs::path src = parent.path() / "src";
    fs::create_directories(src);
    touch(src / "inner.txt");
    auto dest = archive_directory(src, /*dry_run=*/true);
    EXPECT_FALSE(dest.empty());
    EXPECT_TRUE(fs::exists(src));
    EXPECT_FALSE(fs::exists(dest));
}

TEST(ClawCmd, ArchiveDirectory_RealMovesAndRenames) {
    TempDir parent;
    fs::path src = parent.path() / "src2";
    fs::create_directories(src);
    touch(src / "inner.txt", "hello");
    auto dest = archive_directory(src, /*dry_run=*/false);
    ASSERT_FALSE(dest.empty());
    EXPECT_TRUE(fs::exists(dest));
    EXPECT_FALSE(fs::exists(src));
    EXPECT_NE(dest.filename().string().find("archive-"), std::string::npos);
}

TEST(ClawCmd, ArchiveDirectory_MissingSourceReturnsEmpty) {
    TempDir parent;
    auto dest = archive_directory(parent.path() / "never", false);
    EXPECT_TRUE(dest.empty());
}
