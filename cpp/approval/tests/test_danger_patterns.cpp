#include "hermes/approval/danger_patterns.hpp"

#include <gtest/gtest.h>

#include <set>

using hermes::approval::danger_patterns;
using hermes::approval::find_pattern;

TEST(DangerPatterns, AtLeast45Patterns) {
    EXPECT_GE(danger_patterns().size(), 45u);
}

TEST(DangerPatterns, AllKeysUnique) {
    std::set<std::string> seen;
    for (const auto& p : danger_patterns()) {
        EXPECT_TRUE(seen.insert(p.key).second)
            << "duplicate pattern key: " << p.key;
    }
}

TEST(DangerPatterns, AllRegexesNonEmpty) {
    for (const auto& p : danger_patterns()) {
        EXPECT_FALSE(p.regex.empty()) << p.key;
        EXPECT_FALSE(p.description.empty()) << p.key;
        EXPECT_FALSE(p.category.empty()) << p.key;
    }
}

TEST(DangerPatterns, AllSeveritiesInRange) {
    for (const auto& p : danger_patterns()) {
        EXPECT_GE(p.severity, 1) << p.key;
        EXPECT_LE(p.severity, 3) << p.key;
    }
}

TEST(DangerPatterns, CategoriesAreCanonical) {
    const std::set<std::string> ok{"filesystem", "network", "system",
                                   "database", "shell"};
    for (const auto& p : danger_patterns()) {
        EXPECT_TRUE(ok.count(p.category)) << "bad category: " << p.category
                                          << " on key " << p.key;
    }
}

TEST(DangerPatterns, FindPatternByKey) {
    auto rm_root = find_pattern("rm_root");
    ASSERT_TRUE(rm_root.has_value());
    EXPECT_EQ(rm_root->category, "filesystem");

    EXPECT_FALSE(find_pattern("does_not_exist").has_value());
}
