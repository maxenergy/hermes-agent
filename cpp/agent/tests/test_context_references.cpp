#include "hermes/agent/context_references.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>

using hermes::agent::ContextRefKind;
using hermes::agent::ContextReferences;

namespace {

std::filesystem::path make_tempfile(const std::string& body) {
    auto p = std::filesystem::temp_directory_path() /
             ("hermes_ctxref_" +
              std::to_string(
                  std::chrono::system_clock::now().time_since_epoch().count()) +
              ".txt");
    std::ofstream(p) << body;
    return p;
}

}  // namespace

TEST(ContextReferences, StartsEmpty) {
    ContextReferences r;
    EXPECT_TRUE(r.empty());
    EXPECT_EQ(r.size(), 0u);
    EXPECT_EQ(r.render_stable_block(), "");
    EXPECT_EQ(r.drain_per_turn_block(), "");
}

TEST(ContextReferences, RegisterFileStableRenders) {
    auto p = make_tempfile("hello ref body\n");
    ContextReferences r;
    r.register_file(p, /*stable=*/true);
    auto block = r.render_stable_block();
    EXPECT_NE(block.find("## Context references"), std::string::npos);
    EXPECT_NE(block.find("kind=\"file\""), std::string::npos);
    EXPECT_NE(block.find("hello ref body"), std::string::npos);
    EXPECT_NE(block.find(p.filename().string()), std::string::npos);
    std::filesystem::remove(p);
}

TEST(ContextReferences, RegisterFileMissingThrows) {
    ContextReferences r;
    EXPECT_THROW(r.register_file("/no/such/path/should/exist.txt"),
                 std::runtime_error);
    EXPECT_TRUE(r.empty());
}

TEST(ContextReferences, UrlXmlEscaped) {
    ContextReferences r;
    r.register_url("https://example.com/a?x=1&y=<2>", "body");
    auto block = r.render_stable_block();
    EXPECT_NE(block.find("&amp;"), std::string::npos);
    EXPECT_NE(block.find("&lt;2&gt;"), std::string::npos);
    EXPECT_EQ(block.find("&y=<2>"), std::string::npos);
}

TEST(ContextReferences, PerTurnDrainedOnce) {
    ContextReferences r;
    r.register_snippet("id-1", "snippet body", /*stable=*/false);
    auto first = r.drain_per_turn_block();
    EXPECT_NE(first.find("snippet body"), std::string::npos);
    // Second drain should be empty — snippet was consumed.
    auto second = r.drain_per_turn_block();
    EXPECT_EQ(second, "");
    EXPECT_TRUE(r.empty());
}

TEST(ContextReferences, StablePreservedAcrossDrains) {
    auto p = make_tempfile("stable body");
    ContextReferences r;
    r.register_file(p, /*stable=*/true);
    r.register_snippet("vol", "ephemeral body", /*stable=*/false);
    auto drained = r.drain_per_turn_block();
    EXPECT_NE(drained.find("ephemeral body"), std::string::npos);
    EXPECT_EQ(drained.find("stable body"), std::string::npos);
    // Stable ref still visible after drain.
    auto stable = r.render_stable_block();
    EXPECT_NE(stable.find("stable body"), std::string::npos);
    EXPECT_EQ(r.size(), 1u);
    std::filesystem::remove(p);
}

TEST(ContextReferences, RemoveByKindAndSource) {
    ContextReferences r;
    r.register_url("https://a.example/", "A");
    r.register_url("https://b.example/", "B");
    EXPECT_TRUE(r.remove(ContextRefKind::Url, "https://a.example/"));
    EXPECT_FALSE(r.remove(ContextRefKind::Url, "https://missing/"));
    auto block = r.render_stable_block();
    EXPECT_EQ(block.find("https://a.example/"), std::string::npos);
    EXPECT_NE(block.find("https://b.example/"), std::string::npos);
}

TEST(ContextReferences, LargeContentTruncated) {
    std::string big(ContextReferences::kMaxContentBytes + 1000, 'x');
    ContextReferences r;
    r.register_snippet("big", big, /*stable=*/true);
    auto block = r.render_stable_block();
    EXPECT_NE(block.find("[truncated]"), std::string::npos);
    EXPECT_LT(block.size(),
              ContextReferences::kMaxContentBytes + 500);  // drastically smaller
}

TEST(ContextReferences, ClearDropsAll) {
    ContextReferences r;
    r.register_url("https://x/", "a");
    r.register_snippet("s", "b", /*stable=*/false);
    r.clear();
    EXPECT_TRUE(r.empty());
}

TEST(ContextReferences, XmlEscapeAllSpecials) {
    EXPECT_EQ(ContextReferences::xml_escape("a&b<c>d\"e"),
              "a&amp;b&lt;c&gt;d&quot;e");
}
