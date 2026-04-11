#include "hermes/core/patch_parser.hpp"

#include <gtest/gtest.h>
#include <string>

namespace pp = hermes::core::patch_parser;

TEST(PatchParser, SingleFileSingleHunk) {
    const std::string diff =
        "--- a/foo.txt\n"
        "+++ b/foo.txt\n"
        "@@ -1,3 +1,4 @@\n"
        " line1\n"
        "-line2\n"
        "+line2 updated\n"
        "+line2a\n"
        " line3\n";

    const auto files = pp::parse_unified_diff(diff);
    ASSERT_EQ(files.size(), 1U);
    EXPECT_EQ(files[0].old_path, "foo.txt");
    EXPECT_EQ(files[0].new_path, "foo.txt");
    ASSERT_EQ(files[0].hunks.size(), 1U);

    const auto& h = files[0].hunks[0];
    EXPECT_EQ(h.old_start, 1);
    EXPECT_EQ(h.old_count, 3);
    EXPECT_EQ(h.new_start, 1);
    EXPECT_EQ(h.new_count, 4);
    ASSERT_EQ(h.lines.size(), 5U);
    EXPECT_EQ(h.lines[0], " line1");
    EXPECT_EQ(h.lines[1], "-line2");
    EXPECT_EQ(h.lines[2], "+line2 updated");
    EXPECT_EQ(h.lines[3], "+line2a");
    EXPECT_EQ(h.lines[4], " line3");
}

TEST(PatchParser, MissingCountsDefaultToOne) {
    const std::string diff =
        "--- a/x.c\n"
        "+++ b/x.c\n"
        "@@ -42 +42 @@\n"
        "-int x = 1;\n"
        "+int x = 2;\n";
    const auto files = pp::parse_unified_diff(diff);
    ASSERT_EQ(files.size(), 1U);
    ASSERT_EQ(files[0].hunks.size(), 1U);
    EXPECT_EQ(files[0].hunks[0].old_start, 42);
    EXPECT_EQ(files[0].hunks[0].old_count, 1);
    EXPECT_EQ(files[0].hunks[0].new_start, 42);
    EXPECT_EQ(files[0].hunks[0].new_count, 1);
}

TEST(PatchParser, MultipleFiles) {
    const std::string diff =
        "--- a/a.txt\n"
        "+++ b/a.txt\n"
        "@@ -1,1 +1,1 @@\n"
        "-old\n"
        "+new\n"
        "--- a/b.txt\n"
        "+++ b/b.txt\n"
        "@@ -5,2 +5,2 @@\n"
        "-five\n"
        "+FIVE\n"
        " six\n";

    const auto files = pp::parse_unified_diff(diff);
    ASSERT_EQ(files.size(), 2U);
    EXPECT_EQ(files[0].old_path, "a.txt");
    EXPECT_EQ(files[1].old_path, "b.txt");
    EXPECT_EQ(files[1].hunks[0].old_start, 5);
}

TEST(PatchParser, EmptyInputYieldsEmpty) {
    EXPECT_TRUE(pp::parse_unified_diff("").empty());
}

TEST(PatchParser, HeaderWithFunctionSuffix) {
    const std::string diff =
        "--- a/foo.py\n"
        "+++ b/foo.py\n"
        "@@ -10,2 +10,3 @@ def greet():\n"
        " prev\n"
        "+new line\n"
        " next\n";
    const auto files = pp::parse_unified_diff(diff);
    ASSERT_EQ(files.size(), 1U);
    ASSERT_EQ(files[0].hunks.size(), 1U);
    EXPECT_EQ(files[0].hunks[0].old_start, 10);
    EXPECT_EQ(files[0].hunks[0].new_count, 3);
}
