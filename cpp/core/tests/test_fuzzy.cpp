#include "hermes/core/fuzzy.hpp"

#include <gtest/gtest.h>

namespace hf = hermes::core::fuzzy;

TEST(Fuzzy, LevenshteinHappyPath) {
    EXPECT_EQ(hf::levenshtein("kitten", "sitting"), 3);
    EXPECT_EQ(hf::levenshtein("flaw", "lawn"), 2);
    EXPECT_EQ(hf::levenshtein("same", "same"), 0);
}

TEST(Fuzzy, LevenshteinEdgeCases) {
    EXPECT_EQ(hf::levenshtein("", ""), 0);
    EXPECT_EQ(hf::levenshtein("abc", ""), 3);
    EXPECT_EQ(hf::levenshtein("", "xyz"), 3);
}

TEST(Fuzzy, FuzzyContainsHappyPath) {
    EXPECT_TRUE(hf::fuzzy_contains("my favourite model gpt-4o-mini is here", "gpt4o-mini", 2));
    EXPECT_TRUE(hf::fuzzy_contains("hermes agent runner", "hermes", 0));
    EXPECT_FALSE(hf::fuzzy_contains("totally unrelated text", "hermes", 1));
}

TEST(Fuzzy, FuzzyContainsEmptyNeedleAlwaysMatches) {
    EXPECT_TRUE(hf::fuzzy_contains("anything", "", 0));
    EXPECT_TRUE(hf::fuzzy_contains("", "", 3));
}

TEST(Fuzzy, FuzzyContainsNeedleTooLong) {
    EXPECT_FALSE(hf::fuzzy_contains("hi", "hermes", 2));
    EXPECT_TRUE(hf::fuzzy_contains("hermess", "hermes", 1));
}
