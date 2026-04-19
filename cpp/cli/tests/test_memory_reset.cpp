// Upstream: hermes_cli/main.py — ``hermes memory reset`` subcommand
// (commit 3c42064e).  Covers the shared ``memory_reset_impl`` that
// powers both the CLI subcommand and the ``/memory reset`` slash
// command.

#include "hermes/cli/main_entry.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <sstream>
#include <string>

namespace hc = hermes::cli;
namespace fs = std::filesystem;

namespace {

class TempHermesHome {
public:
    TempHermesHome() {
        base_ = fs::temp_directory_path() /
                ("hermes-memreset-" + std::to_string(::getpid()) + "-" +
                 std::to_string(++counter_));
        fs::create_directories(base_ / "memories");
        if (const char* old = std::getenv("HERMES_HOME"); old != nullptr) {
            had_old_ = true;
            old_ = old;
        }
        ::setenv("HERMES_HOME", base_.c_str(), 1);
    }
    ~TempHermesHome() {
        std::error_code ec;
        fs::remove_all(base_, ec);
        if (had_old_) {
            ::setenv("HERMES_HOME", old_.c_str(), 1);
        } else {
            ::unsetenv("HERMES_HOME");
        }
    }
    const fs::path& path() const { return base_; }

    fs::path memories() const { return base_ / "memories"; }

    void write(const std::string& filename, const std::string& body) {
        std::ofstream f(memories() / filename);
        f << body;
    }

private:
    fs::path base_;
    bool had_old_ = false;
    std::string old_;
    static inline int counter_ = 0;
};

}  // namespace

TEST(MemoryReset, RemovesBothFilesWithYesFlag) {
    TempHermesHome home;
    home.write("MEMORY.md", "old agent notes\n");
    home.write("USER.md", "old user profile\n");
    ASSERT_TRUE(fs::exists(home.memories() / "MEMORY.md"));
    ASSERT_TRUE(fs::exists(home.memories() / "USER.md"));

    std::ostringstream out;
    std::istringstream in;
    EXPECT_EQ(hc::memory_reset_impl("all", /*skip_confirmation=*/true, out, in),
              0);

    EXPECT_FALSE(fs::exists(home.memories() / "MEMORY.md"));
    EXPECT_FALSE(fs::exists(home.memories() / "USER.md"));

    const std::string msg = out.str();
    EXPECT_NE(msg.find("Deleted MEMORY.md"), std::string::npos);
    EXPECT_NE(msg.find("Deleted USER.md"), std::string::npos);
    EXPECT_NE(msg.find("Memory reset complete"), std::string::npos);
}

TEST(MemoryReset, ConfirmationDeclineLeavesFilesUntouched) {
    TempHermesHome home;
    home.write("MEMORY.md", "keep me\n");
    home.write("USER.md", "keep me too\n");

    std::ostringstream out;
    // User types "no".
    std::istringstream in("no\n");
    EXPECT_EQ(hc::memory_reset_impl("all", /*skip_confirmation=*/false, out, in),
              0);

    EXPECT_TRUE(fs::exists(home.memories() / "MEMORY.md"));
    EXPECT_TRUE(fs::exists(home.memories() / "USER.md"));
    EXPECT_NE(out.str().find("Cancelled"), std::string::npos);
}

TEST(MemoryReset, ConfirmationAcceptsCaseInsensitiveYes) {
    TempHermesHome home;
    home.write("MEMORY.md", "x\n");

    std::ostringstream out;
    std::istringstream in("  YES  \n");
    EXPECT_EQ(hc::memory_reset_impl("memory", /*skip_confirmation=*/false, out,
                                    in),
              0);

    EXPECT_FALSE(fs::exists(home.memories() / "MEMORY.md"));
}

TEST(MemoryReset, TargetMemoryLeavesUserUntouched) {
    TempHermesHome home;
    home.write("MEMORY.md", "agent\n");
    home.write("USER.md", "user\n");

    std::ostringstream out;
    std::istringstream in;
    EXPECT_EQ(hc::memory_reset_impl("memory", /*skip_confirmation=*/true, out,
                                    in),
              0);

    EXPECT_FALSE(fs::exists(home.memories() / "MEMORY.md"));
    EXPECT_TRUE(fs::exists(home.memories() / "USER.md"));
}

TEST(MemoryReset, TargetUserLeavesMemoryUntouched) {
    TempHermesHome home;
    home.write("MEMORY.md", "agent\n");
    home.write("USER.md", "user\n");

    std::ostringstream out;
    std::istringstream in;
    EXPECT_EQ(
        hc::memory_reset_impl("user", /*skip_confirmation=*/true, out, in), 0);

    EXPECT_TRUE(fs::exists(home.memories() / "MEMORY.md"));
    EXPECT_FALSE(fs::exists(home.memories() / "USER.md"));
}

TEST(MemoryReset, NothingToResetWhenNoFiles) {
    TempHermesHome home;
    // No MEMORY.md or USER.md written.

    std::ostringstream out;
    std::istringstream in;
    EXPECT_EQ(hc::memory_reset_impl("all", /*skip_confirmation=*/true, out, in),
              0);

    EXPECT_NE(out.str().find("Nothing to reset"), std::string::npos);
}

TEST(MemoryReset, UnknownTargetIsAnError) {
    TempHermesHome home;
    home.write("MEMORY.md", "x\n");

    std::ostringstream out;
    std::istringstream in;
    EXPECT_NE(hc::memory_reset_impl("banana", /*skip_confirmation=*/true, out,
                                    in),
              0);

    // File still there — the bad target must not accidentally delete
    // something.
    EXPECT_TRUE(fs::exists(home.memories() / "MEMORY.md"));
}

TEST(MemoryReset, EofOnConfirmationIsCancel) {
    TempHermesHome home;
    home.write("MEMORY.md", "persist\n");

    std::ostringstream out;
    std::istringstream in;  // Empty — getline returns false → cancel.
    EXPECT_EQ(hc::memory_reset_impl("all", /*skip_confirmation=*/false, out,
                                    in),
              0);

    EXPECT_TRUE(fs::exists(home.memories() / "MEMORY.md"));
    EXPECT_NE(out.str().find("Cancelled"), std::string::npos);
}
