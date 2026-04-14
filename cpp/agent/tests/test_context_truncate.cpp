#include "hermes/agent/context_truncate.hpp"

#include <gtest/gtest.h>

using hermes::agent::truncate_context_file;

TEST(ContextTruncate, ShortContentPassesThrough) {
    std::string s(100, 'x');
    EXPECT_EQ(truncate_context_file(s, "AGENTS.md", 500, 0.7, 0.2), s);
}

TEST(ContextTruncate, LongContentTruncated) {
    std::string s(10000, 'x');
    auto out = truncate_context_file(s, "AGENTS.md", 1000, 0.7, 0.2);
    EXPECT_LT(out.size(), s.size());
    EXPECT_NE(out.find("truncated"), std::string::npos);
    EXPECT_NE(out.find("AGENTS.md"), std::string::npos);
}

TEST(ContextTruncate, HeadAndTailPreserved) {
    // Build content with distinct head/tail markers.
    std::string head(500, 'A');
    std::string body(10000, 'B');
    std::string tail(300, 'C');
    std::string content = head + body + tail;
    auto out = truncate_context_file(content, "CLAUDE.md", 1000, 0.7, 0.2);
    EXPECT_EQ(out.front(), 'A');
    EXPECT_EQ(out.back(), 'C');
}

TEST(ContextTruncate, FormatsDroppedCountWithCommas) {
    std::string s(5000, 'x');
    auto out = truncate_context_file(s, ".cursorrules", 1000, 0.7, 0.2);
    EXPECT_NE(out.find(","), std::string::npos);
}

TEST(ContextTruncate, DefaultBudget20k) {
    std::string s(25000, 'x');
    auto out = truncate_context_file(s, "AGENTS.md");
    // Max output ≈ max_chars + header overhead; ensure significantly shorter.
    EXPECT_LT(out.size(), 22000u);
}
