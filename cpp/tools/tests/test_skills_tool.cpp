// Tests for skills_tool — ported from tests/test_skills_tool.py plus
// coverage of the frontmatter / category / tag helpers.
#include "hermes/tools/skills_tool.hpp"
#include "hermes/tools/registry.hpp"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <random>
#include <sstream>

using namespace hermes::tools;
using namespace hermes::tools::skills;
namespace fs = std::filesystem;

namespace {

class SkillsToolTest : public ::testing::Test {
protected:
    void SetUp() override {
        ToolRegistry::instance().clear();
        std::random_device rd;
        std::mt19937 gen(rd());
        std::ostringstream oss;
        oss << "hermes_skills_test_" << gen();
        tmp_ = fs::temp_directory_path() / oss.str();
        fs::create_directories(tmp_);
        setenv("HERMES_HOME", tmp_.c_str(), 1);
        skills::register_skills_tools(ToolRegistry::instance());
    }

    void TearDown() override {
        ToolRegistry::instance().clear();
        fs::remove_all(tmp_);
        unsetenv("HERMES_HOME");
    }

    void write_skill(const fs::path& dir,
                     const std::string& name,
                     const std::string& description,
                     const std::string& body = "# body\n") {
        fs::create_directories(dir);
        std::ofstream ofs(dir / "SKILL.md");
        ofs << "---\n"
            << "name: " << name << "\n"
            << "description: " << description << "\n"
            << "---\n"
            << body;
    }

    std::string dispatch(const std::string& tool,
                         const nlohmann::json& args) {
        return ToolRegistry::instance().dispatch(tool, args, ctx_);
    }

    ToolContext ctx_{"task1", "test", "s1", "cli", "/tmp", {}};
    fs::path tmp_;
};

TEST_F(SkillsToolTest, EmptyDirReturnsEmpty) {
    auto r = nlohmann::json::parse(
        dispatch("skills_list", nlohmann::json::object()));
    EXPECT_TRUE(r["success"].get<bool>());
    EXPECT_EQ(r["count"].get<int>(), 0);
    EXPECT_TRUE(r["skills"].is_array());
    EXPECT_TRUE(r["skills"].empty());
}

TEST_F(SkillsToolTest, LegacyIndexJsonListed) {
    // Python parity — still support a bare index.json skill folder.
    auto dir = tmp_ / "skills" / "legacy";
    fs::create_directories(dir);
    {
        std::ofstream ofs(dir / "index.json");
        ofs << R"({"description": "Legacy"})";
    }
    {
        std::ofstream ofs(dir / "SKILL.md");
        ofs << "# legacy\nBody.\n";
    }
    auto r = nlohmann::json::parse(
        dispatch("skills_list", nlohmann::json::object()));
    EXPECT_EQ(r["count"].get<int>(), 1);
    EXPECT_EQ(r["skills"][0]["name"].get<std::string>(), "legacy");
}

TEST_F(SkillsToolTest, FrontmatterNameDescriptionPreferred) {
    auto dir = tmp_ / "skills" / "folder-name";
    write_skill(dir, "canonical-name", "Frontmatter description");
    auto r = nlohmann::json::parse(
        dispatch("skills_list", nlohmann::json::object()));
    ASSERT_EQ(r["count"].get<int>(), 1);
    EXPECT_EQ(r["skills"][0]["name"].get<std::string>(), "canonical-name");
    EXPECT_EQ(r["skills"][0]["description"].get<std::string>(),
              "Frontmatter description");
}

TEST_F(SkillsToolTest, CategoryFromPath) {
    auto dir = tmp_ / "skills" / "mlops" / "trainer";
    write_skill(dir, "trainer", "Trains things");
    auto r = nlohmann::json::parse(
        dispatch("skills_list", nlohmann::json::object()));
    ASSERT_EQ(r["count"].get<int>(), 1);
    EXPECT_EQ(r["skills"][0]["category"].get<std::string>(), "mlops");

    // Filter by category.
    auto r2 = nlohmann::json::parse(
        dispatch("skills_list", {{"category", "mlops"}}));
    EXPECT_EQ(r2["count"].get<int>(), 1);

    auto r3 = nlohmann::json::parse(
        dispatch("skills_list", {{"category", "other"}}));
    EXPECT_EQ(r3["count"].get<int>(), 0);
}

TEST_F(SkillsToolTest, SkillViewReadsFrontmatterTags) {
    auto dir = tmp_ / "skills" / "tagged";
    fs::create_directories(dir);
    {
        std::ofstream ofs(dir / "SKILL.md");
        ofs << "---\n"
            << "name: tagged\n"
            << "description: with tags\n"
            << "tags: [alpha, beta, gamma]\n"
            << "---\n"
            << "# Body\n"
            << "Hello world.\n";
    }
    auto r = nlohmann::json::parse(
        dispatch("skill_view", {{"name", "tagged"}}));
    EXPECT_EQ(r["name"].get<std::string>(), "tagged");
    EXPECT_TRUE(r["content"].get<std::string>().find("Hello world") !=
                std::string::npos);
    ASSERT_TRUE(r.contains("tags"));
    ASSERT_EQ(r["tags"].size(), 3u);
    EXPECT_EQ(r["tags"][0].get<std::string>(), "alpha");
    EXPECT_EQ(r["tags"][2].get<std::string>(), "gamma");
    EXPECT_GT(r["estimated_tokens"].get<int>(), 0);
}

TEST_F(SkillsToolTest, SkillViewSubfileResolved) {
    auto dir = tmp_ / "skills" / "multi";
    fs::create_directories(dir / "references");
    {
        std::ofstream ofs(dir / "SKILL.md");
        ofs << "# multi\n";
    }
    {
        std::ofstream ofs(dir / "references" / "api.md");
        ofs << "# API\nReference.\n";
    }
    auto r = nlohmann::json::parse(dispatch(
        "skill_view",
        {{"name", "multi"}, {"file", "references/api.md"}}));
    EXPECT_EQ(r["file"].get<std::string>(), "references/api.md");
    EXPECT_TRUE(r["content"].get<std::string>().find("Reference") !=
                std::string::npos);
}

TEST_F(SkillsToolTest, SkillViewRejectsDotDot) {
    auto dir = tmp_ / "skills" / "secured";
    write_skill(dir, "secured", "no escape");
    auto s = dispatch("skill_view",
                      {{"name", "secured"}, {"file", "../../etc/passwd"}});
    auto j = nlohmann::json::parse(s);
    EXPECT_TRUE(j.contains("error"));
}

TEST_F(SkillsToolTest, SkillViewMissingSkill) {
    auto s = dispatch("skill_view", {{"name", "does-not-exist"}});
    auto j = nlohmann::json::parse(s);
    EXPECT_TRUE(j.contains("error"));
}

TEST_F(SkillsToolTest, SkillsCategoriesCounts) {
    write_skill(tmp_ / "skills" / "ops" / "alpha", "alpha", "A");
    write_skill(tmp_ / "skills" / "ops" / "beta",  "beta",  "B");
    write_skill(tmp_ / "skills" / "docs" / "one",  "one",   "C");

    auto r = nlohmann::json::parse(
        dispatch("skills_categories", nlohmann::json::object()));
    ASSERT_TRUE(r["success"].get<bool>());
    ASSERT_EQ(r["categories"].size(), 2u);
    // categories are sorted: docs, ops
    EXPECT_EQ(r["categories"][0]["name"].get<std::string>(), "docs");
    EXPECT_EQ(r["categories"][0]["skill_count"].get<int>(), 1);
    EXPECT_EQ(r["categories"][1]["name"].get<std::string>(), "ops");
    EXPECT_EQ(r["categories"][1]["skill_count"].get<int>(), 2);
}

TEST_F(SkillsToolTest, SkillsCategoriesDescriptionMd) {
    write_skill(tmp_ / "skills" / "ops" / "alpha", "alpha", "A");
    {
        std::ofstream ofs(tmp_ / "skills" / "ops" / "DESCRIPTION.md");
        ofs << "---\n"
            << "description: Operations skills.\n"
            << "---\n";
    }
    auto r = nlohmann::json::parse(
        dispatch("skills_categories", nlohmann::json::object()));
    ASSERT_EQ(r["categories"].size(), 1u);
    EXPECT_EQ(r["categories"][0]["description"].get<std::string>(),
              "Operations skills.");
}

TEST_F(SkillsToolTest, ExcludedDirsIgnored) {
    write_skill(tmp_ / "skills" / "node_modules" / "junk", "junk", "no");
    write_skill(tmp_ / "skills" / "real", "real", "yes");
    auto r = nlohmann::json::parse(
        dispatch("skills_list", nlohmann::json::object()));
    EXPECT_EQ(r["count"].get<int>(), 1);
    EXPECT_EQ(r["skills"][0]["name"].get<std::string>(), "real");
}

// ----- Helper function coverage ---------------------------------------

TEST(SkillsUtil, ParseFrontmatterBasic) {
    auto [fm, body] = parse_frontmatter(
        "---\nname: foo\ndescription: bar\n---\n# body\nBody text.");
    EXPECT_EQ(fm["name"].get<std::string>(), "foo");
    EXPECT_EQ(fm["description"].get<std::string>(), "bar");
    EXPECT_TRUE(body.find("Body text") != std::string::npos);
}

TEST(SkillsUtil, ParseFrontmatterMissing) {
    auto [fm, body] = parse_frontmatter("no frontmatter here");
    EXPECT_TRUE(fm.empty());
    EXPECT_EQ(body, "no frontmatter here");
}

TEST(SkillsUtil, ParseFrontmatterInlineList) {
    auto [fm, _] = parse_frontmatter(
        "---\nplatforms: [linux, macos]\n---\n");
    ASSERT_TRUE(fm["platforms"].is_array());
    EXPECT_EQ(fm["platforms"].size(), 2u);
    EXPECT_EQ(fm["platforms"][0].get<std::string>(), "linux");
}

TEST(SkillsUtil, ParseFrontmatterBlockList) {
    auto [fm, _] = parse_frontmatter(
        "---\nplatforms:\n  - linux\n  - macos\n---\n");
    ASSERT_TRUE(fm["platforms"].is_array());
    EXPECT_EQ(fm["platforms"].size(), 2u);
}

TEST(SkillsUtil, ParseTagsList) {
    auto t = parse_tags(nlohmann::json::array({"a", "b", "c"}));
    ASSERT_EQ(t.size(), 3u);
    EXPECT_EQ(t[0], "a");
}

TEST(SkillsUtil, ParseTagsBracketString) {
    auto t = parse_tags(nlohmann::json("[foo, \"bar\", 'baz']"));
    ASSERT_EQ(t.size(), 3u);
    EXPECT_EQ(t[1], "bar");
    EXPECT_EQ(t[2], "baz");
}

TEST(SkillsUtil, EstimateTokens) {
    EXPECT_EQ(estimate_tokens(""), 0u);
    EXPECT_EQ(estimate_tokens("abcd"), 1u);
    EXPECT_EQ(estimate_tokens("aaaaaaaaaaaaaaaa"), 4u);
}

TEST(SkillsUtil, SkillMatchesPlatformDefault) {
    nlohmann::json fm = nlohmann::json::object();
    EXPECT_TRUE(skill_matches_platform(fm));
}

TEST(SkillsUtil, SkillMatchesPlatformArray) {
    nlohmann::json fm = {{"platforms",
                          nlohmann::json::array({"linux", "macos", "windows"})}};
    EXPECT_TRUE(skill_matches_platform(fm));
    nlohmann::json none = {{"platforms", nlohmann::json::array({"plan9"})}};
    EXPECT_FALSE(skill_matches_platform(none));
}

}  // namespace
