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

// ── sanitize_surrogates — ports upstream Python commit 8798b069. ──────────

TEST(StringsSurrogate, CleanStringUnchanged) {
    // ASCII — no 0xED byte.
    const std::string ascii = "hello world";
    EXPECT_FALSE(strs::contains_surrogate(ascii));
    EXPECT_EQ(strs::sanitize_surrogates(ascii), ascii);

    // CJK — multi-byte but no surrogate encoding.
    const std::string cjk = "你好世界";
    EXPECT_FALSE(strs::contains_surrogate(cjk));
    EXPECT_EQ(strs::sanitize_surrogates(cjk), cjk);

    // Emoji — 4-byte UTF-8.
    const std::string emoji = "\xF0\x9F\x98\x80 smile";  // U+1F600
    EXPECT_FALSE(strs::contains_surrogate(emoji));
    EXPECT_EQ(strs::sanitize_surrogates(emoji), emoji);
}

TEST(StringsSurrogate, LoneHighSurrogateReplacedWithFffd) {
    // Lone U+D83D (high surrogate) encoded as ED A0 BD.
    const std::string bad = std::string("pre") +
                            "\xED\xA0\xBD" +
                            std::string("post");
    EXPECT_TRUE(strs::contains_surrogate(bad));
    // U+FFFD = EF BF BD.
    EXPECT_EQ(strs::sanitize_surrogates(bad), "pre\xEF\xBF\xBDpost");
}

TEST(StringsSurrogate, LoneLowSurrogateReplacedWithFffd) {
    // Lone U+DC00 (low surrogate) encoded as ED B0 80.
    const std::string bad = "x\xED\xB0\x80y";
    EXPECT_TRUE(strs::contains_surrogate(bad));
    EXPECT_EQ(strs::sanitize_surrogates(bad), "x\xEF\xBF\xBDy");
}

TEST(StringsSurrogate, ValidThreeByteEDSequenceNotReplaced) {
    // U+D000 encoded as ED 80 80 — byte1 below surrogate range 0xA0..0xBF.
    const std::string valid = "\xED\x80\x80";
    EXPECT_FALSE(strs::contains_surrogate(valid));
    EXPECT_EQ(strs::sanitize_surrogates(valid), valid);

    // U+D7FF (last valid BMP before surrogates) = ED 9F BF.
    const std::string near_edge = "\xED\x9F\xBF";
    EXPECT_FALSE(strs::contains_surrogate(near_edge));
    EXPECT_EQ(strs::sanitize_surrogates(near_edge), near_edge);
}

TEST(StringsSurrogate, MultipleSurrogatesAllReplaced) {
    const std::string bad = std::string("a") + "\xED\xA0\xBD" +
                            "b" + "\xED\xB0\x80" + "c";
    EXPECT_EQ(strs::sanitize_surrogates(bad),
              std::string("a") + "\xEF\xBF\xBD" + "b" + "\xEF\xBF\xBD" + "c");
}

TEST(StringsSurrogate, SurrogateMixedWithCjkAndEmoji) {
    // Make sure we don't damage valid multi-byte sequences next to bad ones.
    const std::string bad = "你\xED\xA0\xBD\xF0\x9F\x98\x80好";
    EXPECT_EQ(strs::sanitize_surrogates(bad),
              "你\xEF\xBF\xBD\xF0\x9F\x98\x80好");
}

TEST(StringsSurrogate, TruncatedEDByteNoCrash) {
    // Dangling 0xED at end-of-string with no continuation bytes.
    const std::string partial = "abc\xED";
    EXPECT_FALSE(strs::contains_surrogate(partial));
    // Passthrough — non-surrogate truncation is not our job to fix.
    EXPECT_EQ(strs::sanitize_surrogates(partial), partial);
}

TEST(StringsSurrogate, EmptyInput) {
    EXPECT_FALSE(strs::contains_surrogate(""));
    EXPECT_EQ(strs::sanitize_surrogates(""), "");
}
