// Sanity checks for the shipped default SOUL/BOOT assets.
#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;

namespace {

#ifndef HERMES_SOURCE_ASSETS_DIR
#define HERMES_SOURCE_ASSETS_DIR "."
#endif

std::string read(const fs::path& p) {
    std::ifstream in(p);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

}  // namespace

TEST(DefaultAssets, SoulIsSubstantial) {
    fs::path p = fs::path(HERMES_SOURCE_ASSETS_DIR) / "default_soul.md";
    ASSERT_TRUE(fs::exists(p)) << p;
    auto content = read(p);
    EXPECT_GT(content.size(), 100u);
    EXPECT_NE(content.find("Hermes"), std::string::npos);
}

TEST(DefaultAssets, BootIsNonEmpty) {
    fs::path p = fs::path(HERMES_SOURCE_ASSETS_DIR) / "default_boot.md";
    ASSERT_TRUE(fs::exists(p)) << p;
    auto content = read(p);
    EXPECT_GT(content.size(), 20u);
}
