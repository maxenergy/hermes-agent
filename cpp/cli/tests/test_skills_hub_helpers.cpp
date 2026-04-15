// Tests for skills_hub_helpers — pure-logic port of hermes_cli/skills_hub.py.
#include "hermes/cli/skills_hub_helpers.hpp"

#include <gtest/gtest.h>

namespace sh = hermes::cli::skills_hub_helpers;

// ---------------------------------------------------------------------------
// Trust ranking.
// ---------------------------------------------------------------------------

TEST(SkillsHubTrust, Rank) {
    EXPECT_EQ(sh::trust_rank("builtin"), 3);
    EXPECT_EQ(sh::trust_rank("trusted"), 2);
    EXPECT_EQ(sh::trust_rank("community"), 1);
    EXPECT_EQ(sh::trust_rank("unknown"), 0);
    EXPECT_EQ(sh::trust_rank(""), 0);
}

TEST(SkillsHubTrust, Style) {
    EXPECT_EQ(sh::trust_style("builtin"), "bright_cyan");
    EXPECT_EQ(sh::trust_style("trusted"), "green");
    EXPECT_EQ(sh::trust_style("community"), "yellow");
    EXPECT_EQ(sh::trust_style("other"), "dim");
}

TEST(SkillsHubTrust, Label) {
    EXPECT_EQ(sh::trust_label("official", "community"), "official");
    EXPECT_EQ(sh::trust_label("github", "community"), "community");
    EXPECT_EQ(sh::trust_label("skills-sh", "trusted"), "trusted");
}

// ---------------------------------------------------------------------------
// Category derivation.
// ---------------------------------------------------------------------------

TEST(SkillsHubCategory, Empty) {
    EXPECT_EQ(sh::derive_category_from_install_path(""), "");
    EXPECT_EQ(sh::derive_category_from_install_path("skill.md"), "");
}

TEST(SkillsHubCategory, SingleDir) {
    EXPECT_EQ(sh::derive_category_from_install_path("tools/foo"), "tools");
}

TEST(SkillsHubCategory, Nested) {
    EXPECT_EQ(sh::derive_category_from_install_path("a/b/c/SKILL.md"),
              "a/b/c");
}

// ---------------------------------------------------------------------------
// Per-source limits.
// ---------------------------------------------------------------------------

TEST(SkillsHubLimits, Known) {
    EXPECT_EQ(sh::per_source_limit("official"), 200);
    EXPECT_EQ(sh::per_source_limit("well-known"), 50);
    EXPECT_EQ(sh::per_source_limit("lobehub"), 500);
}

TEST(SkillsHubLimits, Unknown) {
    EXPECT_EQ(sh::per_source_limit("blog"), 100);
}

// ---------------------------------------------------------------------------
// Pagination.
// ---------------------------------------------------------------------------

TEST(SkillsHubPaginate, EmptyList) {
    const auto w {sh::paginate(0, 1, 20)};
    EXPECT_EQ(w.total_pages, 1u);
    EXPECT_EQ(w.page, 1u);
    EXPECT_EQ(w.start, 0u);
    EXPECT_EQ(w.end, 0u);
}

TEST(SkillsHubPaginate, FirstPage) {
    const auto w {sh::paginate(50, 1, 20)};
    EXPECT_EQ(w.total_pages, 3u);
    EXPECT_EQ(w.page, 1u);
    EXPECT_EQ(w.start, 0u);
    EXPECT_EQ(w.end, 20u);
    EXPECT_EQ(w.page_size, 20u);
}

TEST(SkillsHubPaginate, LastPage) {
    const auto w {sh::paginate(50, 3, 20)};
    EXPECT_EQ(w.total_pages, 3u);
    EXPECT_EQ(w.start, 40u);
    EXPECT_EQ(w.end, 50u);
}

TEST(SkillsHubPaginate, ClampsOverflowPage) {
    const auto w {sh::paginate(10, 999, 5)};
    EXPECT_EQ(w.page, 2u);
    EXPECT_EQ(w.total_pages, 2u);
}

TEST(SkillsHubPaginate, ClampsPageSize) {
    const auto big {sh::paginate(250, 1, 9999)};
    EXPECT_EQ(big.page_size, 100u);
    const auto tiny {sh::paginate(10, 1, 0)};
    EXPECT_EQ(tiny.page_size, 1u);
}

// ---------------------------------------------------------------------------
// Extra metadata formatting.
// ---------------------------------------------------------------------------

TEST(SkillsHubExtra, EmptyYieldsNothing) {
    sh::ExtraMetadata m {};
    EXPECT_TRUE(sh::format_extra_metadata_lines(m).empty());
}

TEST(SkillsHubExtra, OrderedFields) {
    sh::ExtraMetadata m {};
    m.repo_url = "https://github.com/x/y";
    m.detail_url = "https://example/y";
    m.endpoint = "https://api/x";
    m.installs = 42;
    m.weekly_installs = "7";
    m.security_audits = {{"snyk", "pass"}, {"audit", "fail"}};
    const auto lines {sh::format_extra_metadata_lines(m)};
    ASSERT_EQ(lines.size(), 6u);
    EXPECT_NE(lines[0].find("Repo:"), std::string::npos);
    EXPECT_NE(lines[1].find("Detail Page:"), std::string::npos);
    EXPECT_NE(lines[2].find("Endpoint:"), std::string::npos);
    EXPECT_NE(lines[3].find("Installs:"), std::string::npos);
    EXPECT_NE(lines[3].find("42"), std::string::npos);
    EXPECT_NE(lines[4].find("Weekly Installs:"), std::string::npos);
    EXPECT_NE(lines[5].find("audit=fail, snyk=pass"), std::string::npos);
}

// ---------------------------------------------------------------------------
// Dedup / sort.
// ---------------------------------------------------------------------------

TEST(SkillsHubDedup, KeepsHigherTrust) {
    std::vector<sh::Result> in {
        {"foo", "a/foo", "github", "community", ""},
        {"foo", "b/foo", "official", "builtin", ""},
        {"bar", "c/bar", "github", "trusted", ""},
    };
    const auto out {sh::deduplicate_by_name(in)};
    ASSERT_EQ(out.size(), 2u);
    // foo should have builtin trust
    for (const auto& r : out) {
        if (r.name == "foo") {
            EXPECT_EQ(r.trust_level, "builtin");
        }
    }
}

TEST(SkillsHubSort, OfficialFirstThenTrust) {
    std::vector<sh::Result> r {
        {"zzz", "1", "github", "community", ""},
        {"aaa", "2", "official", "builtin", ""},
        {"mmm", "3", "github", "trusted", ""},
    };
    sh::sort_browse_results(r);
    EXPECT_EQ(r[0].name, "aaa");  // builtin + official
    EXPECT_EQ(r[1].name, "mmm");  // trusted
    EXPECT_EQ(r[2].name, "zzz");  // community
}

TEST(SkillsHubSort, AlphabeticalWithinTier) {
    std::vector<sh::Result> r {
        {"Bravo", "", "github", "community", ""},
        {"alpha", "", "github", "community", ""},
    };
    sh::sort_browse_results(r);
    EXPECT_EQ(r[0].name, "alpha");
    EXPECT_EQ(r[1].name, "Bravo");
}

// ---------------------------------------------------------------------------
// Resolution classification.
// ---------------------------------------------------------------------------

TEST(SkillsHubResolve, NoMatch) {
    EXPECT_EQ(sh::classify_resolution({}), sh::ResolveOutcome::NoMatch);
}

TEST(SkillsHubResolve, Exact) {
    EXPECT_EQ(sh::classify_resolution({{"x", "a/x", "", "", ""}}),
              sh::ResolveOutcome::Exact);
}

TEST(SkillsHubResolve, Ambiguous) {
    EXPECT_EQ(sh::classify_resolution({{"x", "a/x", "", "", ""},
                                       {"x", "b/x", "", "", ""}}),
              sh::ResolveOutcome::Ambiguous);
}

// ---------------------------------------------------------------------------
// Description truncation.
// ---------------------------------------------------------------------------

TEST(SkillsHubTruncate, NoChange) {
    EXPECT_EQ(sh::truncate_description("short", 20), "short");
}

TEST(SkillsHubTruncate, EllipsisAdded) {
    EXPECT_EQ(sh::truncate_description("0123456789ABC", 5),
              "01234...");
}

TEST(SkillsHubTruncate, ExactLength) {
    EXPECT_EQ(sh::truncate_description("12345", 5), "12345");
}

// ---------------------------------------------------------------------------
// Tap actions.
// ---------------------------------------------------------------------------

TEST(SkillsHubTap, Valid) {
    EXPECT_TRUE(sh::is_valid_tap_action("add"));
    EXPECT_TRUE(sh::is_valid_tap_action("remove"));
    EXPECT_TRUE(sh::is_valid_tap_action("list"));
    EXPECT_TRUE(sh::is_valid_tap_action("update"));
}

TEST(SkillsHubTap, Invalid) {
    EXPECT_FALSE(sh::is_valid_tap_action(""));
    EXPECT_FALSE(sh::is_valid_tap_action("del"));
    EXPECT_FALSE(sh::is_valid_tap_action("ADD"));
}

// ---------------------------------------------------------------------------
// Source labels & filter validation.
// ---------------------------------------------------------------------------

TEST(SkillsHubSourceLabel, Known) {
    EXPECT_EQ(sh::source_label("official"), "Nous Research (official)");
    EXPECT_EQ(sh::source_label("skills-sh"), "skills.sh");
    EXPECT_EQ(sh::source_label("github"), "GitHub");
    EXPECT_EQ(sh::source_label("clawhub"), "ClawHub");
    EXPECT_EQ(sh::source_label("lobehub"), "LobeHub");
    EXPECT_EQ(sh::source_label("well-known"), ".well-known");
    EXPECT_EQ(sh::source_label("claude-marketplace"), "Claude Marketplace");
}

TEST(SkillsHubSourceLabel, Unknown) {
    EXPECT_EQ(sh::source_label("bogus"), "bogus");
}

TEST(SkillsHubFilter, Valid) {
    EXPECT_TRUE(sh::is_valid_source_filter("all"));
    EXPECT_TRUE(sh::is_valid_source_filter("official"));
    EXPECT_TRUE(sh::is_valid_source_filter("lobehub"));
}

TEST(SkillsHubFilter, Invalid) {
    EXPECT_FALSE(sh::is_valid_source_filter(""));
    EXPECT_FALSE(sh::is_valid_source_filter("some-hub"));
}

// ---------------------------------------------------------------------------
// Header & status formatting.
// ---------------------------------------------------------------------------

TEST(SkillsHubHeader, Search) {
    EXPECT_NE(sh::search_header(5).find("5 result(s)"), std::string::npos);
}

TEST(SkillsHubHeader, BrowseAll) {
    const std::string s {sh::browse_status_line(100, 2, 5, "all", 0)};
    EXPECT_NE(s.find("all sources"), std::string::npos);
    EXPECT_NE(s.find("100 skills loaded"), std::string::npos);
    EXPECT_NE(s.find("page 2/5"), std::string::npos);
}

TEST(SkillsHubHeader, BrowseWithTimeouts) {
    const std::string s {
        sh::browse_status_line(50, 1, 3, "official", 2)};
    EXPECT_NE(s.find("official"), std::string::npos);
    EXPECT_NE(s.find("2 source(s) still loading"), std::string::npos);
}
