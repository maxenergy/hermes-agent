// Tests for hermes::agent::atref:: @-reference parser.
#include "hermes/agent/at_references.hpp"

#include <gtest/gtest.h>

using namespace hermes::agent::atref;

TEST(AtReferences, ParsesDiff) {
    auto r = parse_at_references("please @diff the repo");
    ASSERT_EQ(r.size(), 1u);
    EXPECT_EQ(r[0].kind, RefKind::Diff);
    EXPECT_EQ(r[0].raw, "@diff");
}

TEST(AtReferences, ParsesStagedAtStart) {
    auto r = parse_at_references("@staged");
    ASSERT_EQ(r.size(), 1u);
    EXPECT_EQ(r[0].kind, RefKind::Staged);
}

TEST(AtReferences, SkipsInsideWord) {
    // Must be preceded by non-word/non-slash.
    auto r = parse_at_references("foo@diff");
    EXPECT_TRUE(r.empty());
}

TEST(AtReferences, ParsesFileWithLineRange) {
    auto r = parse_at_references("check @file:src/main.py:10-20 please");
    ASSERT_EQ(r.size(), 1u);
    EXPECT_EQ(r[0].kind, RefKind::File);
    EXPECT_EQ(r[0].target, "src/main.py");
    EXPECT_EQ(r[0].line_start.value_or(0), 10);
    EXPECT_EQ(r[0].line_end.value_or(0), 20);
}

TEST(AtReferences, ParsesFileWithSingleLine) {
    auto r = parse_at_references("@file:a.py:42");
    ASSERT_EQ(r.size(), 1u);
    EXPECT_EQ(r[0].target, "a.py");
    EXPECT_EQ(r[0].line_start.value_or(0), 42);
    EXPECT_EQ(r[0].line_end.value_or(0), 42);
}

TEST(AtReferences, ParsesQuotedPathWithSpaces) {
    auto r = parse_at_references(R"(@file:"path with space.py":1-3)");
    ASSERT_EQ(r.size(), 1u);
    EXPECT_EQ(r[0].target, "path with space.py");
    EXPECT_EQ(r[0].line_start.value_or(0), 1);
    EXPECT_EQ(r[0].line_end.value_or(0), 3);
}

TEST(AtReferences, ParsesFolder) {
    auto r = parse_at_references("look at @folder:src");
    ASSERT_EQ(r.size(), 1u);
    EXPECT_EQ(r[0].kind, RefKind::Folder);
    EXPECT_EQ(r[0].target, "src");
}

TEST(AtReferences, ParsesGitWithCount) {
    auto r = parse_at_references("show @git:3 commits");
    ASSERT_EQ(r.size(), 1u);
    EXPECT_EQ(r[0].kind, RefKind::Git);
    EXPECT_EQ(r[0].target, "3");
}

TEST(AtReferences, ParsesUrl) {
    auto r = parse_at_references("fetch @url:https://example.com/docs");
    ASSERT_EQ(r.size(), 1u);
    EXPECT_EQ(r[0].kind, RefKind::Url);
    EXPECT_EQ(r[0].target, "https://example.com/docs");
}

TEST(AtReferences, StripsTrailingPunctuation) {
    auto r = parse_at_references("see @file:a.py.");
    ASSERT_EQ(r.size(), 1u);
    EXPECT_EQ(r[0].target, "a.py");
}

TEST(AtReferences, MultipleReferencesInOneMessage) {
    auto r = parse_at_references("@file:a.py and @folder:src and @diff");
    ASSERT_EQ(r.size(), 3u);
    EXPECT_EQ(r[0].kind, RefKind::File);
    EXPECT_EQ(r[1].kind, RefKind::Folder);
    EXPECT_EQ(r[2].kind, RefKind::Diff);
}

TEST(AtReferences, RemoveTokensCleansText) {
    std::string msg = "please @file:a.py and @diff, thanks.";
    auto refs = parse_at_references(msg);
    auto cleaned = remove_reference_tokens(msg, refs);
    EXPECT_EQ(cleaned, "please and, thanks.");
}

TEST(AtReferencesDetail, StripTrailingPunctuation) {
    EXPECT_EQ(detail::strip_trailing_punctuation("path!"), "path");
    EXPECT_EQ(detail::strip_trailing_punctuation("path)"), "path");
    EXPECT_EQ(detail::strip_trailing_punctuation("path(a)"), "path(a)");
}

TEST(AtReferencesDetail, StripWrappers) {
    EXPECT_EQ(detail::strip_reference_wrappers("`a.py`"), "a.py");
    EXPECT_EQ(detail::strip_reference_wrappers("\"a.py\""), "a.py");
    EXPECT_EQ(detail::strip_reference_wrappers("a.py"), "a.py");
}

TEST(AtReferencesDetail, KindName) {
    EXPECT_EQ(kind_name(RefKind::File), "file");
    EXPECT_EQ(kind_name(RefKind::Url), "url");
    EXPECT_EQ(kind_name(RefKind::Unknown), "unknown");
}

TEST(AtReferencesDetail, ParseFileReferenceValue) {
    std::string path;
    std::optional<int> s, e;
    detail::parse_file_reference_value("a.py", path, s, e);
    EXPECT_EQ(path, "a.py");
    EXPECT_FALSE(s.has_value());
    detail::parse_file_reference_value("a.py:5", path, s, e);
    EXPECT_EQ(path, "a.py");
    EXPECT_EQ(*s, 5);
    EXPECT_EQ(*e, 5);
    detail::parse_file_reference_value("a.py:5-9", path, s, e);
    EXPECT_EQ(*e, 9);
    detail::parse_file_reference_value("`b.py`:1-2", path, s, e);
    EXPECT_EQ(path, "b.py");
    EXPECT_EQ(*s, 1);
    EXPECT_EQ(*e, 2);
}
