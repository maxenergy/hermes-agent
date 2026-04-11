#include "hermes/core/strings.hpp"

#include <gtest/gtest.h>

namespace strs = hermes::core::strings;

TEST(Strings, SplitHappyPath) {
    const auto parts = strs::split("a,b,c,d", ",");
    ASSERT_EQ(parts.size(), 4U);
    EXPECT_EQ(parts[0], "a");
    EXPECT_EQ(parts[3], "d");
}

TEST(Strings, SplitKeepsEmptySegments) {
    const auto parts = strs::split(",,x,", ",");
    ASSERT_EQ(parts.size(), 4U);
    EXPECT_EQ(parts[0], "");
    EXPECT_EQ(parts[1], "");
    EXPECT_EQ(parts[2], "x");
    EXPECT_EQ(parts[3], "");
}

TEST(Strings, SplitEmptyDelimReturnsInput) {
    const auto parts = strs::split("hello", "");
    ASSERT_EQ(parts.size(), 1U);
    EXPECT_EQ(parts[0], "hello");
}

TEST(Strings, JoinHappyPath) {
    EXPECT_EQ(strs::join({"a", "b", "c"}, "-"), "a-b-c");
    EXPECT_EQ(strs::join({}, ","), "");
    EXPECT_EQ(strs::join({"only"}, ","), "only");
}

TEST(Strings, StartsAndEndsWith) {
    EXPECT_TRUE(strs::starts_with("hello world", "hello"));
    EXPECT_FALSE(strs::starts_with("hi", "hello"));
    EXPECT_TRUE(strs::ends_with("file.txt", ".txt"));
    EXPECT_FALSE(strs::ends_with("file.tx", ".txt"));
}

TEST(Strings, TrimEdgeCases) {
    EXPECT_EQ(strs::trim("  hi  "), "hi");
    EXPECT_EQ(strs::trim(""), "");
    EXPECT_EQ(strs::trim("\t\nhi\r\n"), "hi");
    EXPECT_EQ(strs::trim("no-trim"), "no-trim");
}

TEST(Strings, CaseTransforms) {
    EXPECT_EQ(strs::to_lower("AbCdE"), "abcde");
    EXPECT_EQ(strs::to_upper("AbCdE"), "ABCDE");
    EXPECT_EQ(strs::to_lower(""), "");
}

TEST(Strings, ContainsBoundary) {
    EXPECT_TRUE(strs::contains("hello world", "world"));
    EXPECT_FALSE(strs::contains("hello", "WORLD"));
    EXPECT_TRUE(strs::contains("anything", ""));  // empty needle always matches
}
