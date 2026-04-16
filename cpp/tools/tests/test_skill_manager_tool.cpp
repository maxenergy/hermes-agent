#include "hermes/tools/skill_manager_tool.hpp"
#include "hermes/tools/registry.hpp"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <random>
#include <sstream>

using namespace hermes::tools;
using json = nlohmann::json;
namespace fs = std::filesystem;

namespace {

class SkillManagerToolTest : public ::testing::Test {
protected:
    void SetUp() override {
        ToolRegistry::instance().clear();
        std::random_device rd;
        std::mt19937 gen(rd());
        std::ostringstream oss;
        oss << "hermes_skill_mgr_test_" << gen();
        tmp_ = fs::temp_directory_path() / oss.str();
        fs::create_directories(tmp_);
        setenv("HERMES_HOME", tmp_.c_str(), 1);
        register_skill_manager_tools(ToolRegistry::instance());
    }

    void TearDown() override {
        ToolRegistry::instance().clear();
        fs::remove_all(tmp_);
        unsetenv("HERMES_HOME");
    }

    std::string dispatch(const std::string& tool, const json& args) {
        return ToolRegistry::instance().dispatch(tool, args, ctx_);
    }

    ToolContext ctx_{"task1", "test", "s1", "cli", "/tmp", {}};
    fs::path tmp_;
};

TEST_F(SkillManagerToolTest, ListInstalledEmpty) {
    auto r = json::parse(dispatch("skill_manage",
        {{"action", "list_installed"}}));
    EXPECT_EQ(r["count"].get<int>(), 0);
    EXPECT_TRUE(r["skills"].is_array());
    EXPECT_TRUE(r["skills"].empty());
}

TEST_F(SkillManagerToolTest, UninstallSafetyCheck) {
    // Path traversal attack should be rejected.
    auto r1 = json::parse(dispatch("skill_manage",
        {{"action", "uninstall"}, {"name", "../escape"}}));
    EXPECT_TRUE(r1.contains("error"));

    auto r2 = json::parse(dispatch("skill_manage",
        {{"action", "uninstall"}, {"name", ".."}}));
    EXPECT_TRUE(r2.contains("error"));

    auto r3 = json::parse(dispatch("skill_manage",
        {{"action", "uninstall"}, {"name", "nonexistent"}}));
    EXPECT_TRUE(r3.contains("error"));
}

TEST_F(SkillManagerToolTest, SearchFallsBackToLocalScan) {
    // With no HERMES_SKILLS_HUB_URL configured the handler now falls
    // back to a local installed-skills scan; it should return a
    // well-formed (possibly empty) list rather than an error.
    auto r = json::parse(dispatch("skill_manage",
        {{"action", "search"}, {"query", "something-unlikely-xyz"}}));
    EXPECT_FALSE(r.contains("error"));
    EXPECT_TRUE(r.contains("skills"));
    EXPECT_TRUE(r.contains("count"));
}

TEST_F(SkillManagerToolTest, SearchRejectsEmptyQuery) {
    auto r = json::parse(dispatch("skill_manage",
        {{"action", "search"}, {"query", ""}}));
    EXPECT_TRUE(r.contains("error"));
}

TEST_F(SkillManagerToolTest, UninstallExistingSkill) {
    // Create a skill directory.
    auto skill_dir = tmp_ / "skills" / "test-skill";
    fs::create_directories(skill_dir);
    {
        std::ofstream ofs(skill_dir / "SKILL.md");
        ofs << "# Test Skill";
    }

    auto r = json::parse(dispatch("skill_manage",
        {{"action", "uninstall"}, {"name", "test-skill"}}));
    EXPECT_TRUE(r["uninstalled"].get<bool>());
    EXPECT_FALSE(fs::exists(skill_dir));
}

TEST_F(SkillManagerToolTest, ListInstalledIncludesDescription) {
    auto dir = tmp_ / "skills" / "demo";
    fs::create_directories(dir);
    {
        std::ofstream md(dir / "SKILL.md");
        md << "---\nname: demo\ndescription: A demo skill\nversion: 1.2.3\n---\nbody";
    }
    auto r = json::parse(dispatch("skill_manage", {{"action", "list_installed"}}));
    ASSERT_EQ(r["count"].get<int>(), 1);
    EXPECT_EQ(r["skills"][0]["name"].get<std::string>(), "demo");
    EXPECT_EQ(r["skills"][0]["description"].get<std::string>(), "A demo skill");
    EXPECT_EQ(r["skills"][0]["version"].get<std::string>(), "1.2.3");
}

TEST_F(SkillManagerToolTest, ListInstalledIndexJsonWins) {
    auto dir = tmp_ / "skills" / "alpha";
    fs::create_directories(dir);
    {
        std::ofstream j(dir / "index.json");
        j << R"({"description":"from index","version":"9.9"})";
    }
    auto r = json::parse(dispatch("skill_manage", {{"action", "list_installed"}}));
    EXPECT_EQ(r["skills"][0]["description"].get<std::string>(), "from index");
}

TEST_F(SkillManagerToolTest, SearchAcrossInstalled) {
    auto a = tmp_ / "skills" / "deploy-staging";
    auto b = tmp_ / "skills" / "deploy-prod";
    fs::create_directories(a);
    fs::create_directories(b);
    { std::ofstream(a / "SKILL.md") << "stub"; }
    { std::ofstream(b / "SKILL.md") << "stub"; }
    auto r = json::parse(dispatch("skill_manage",
        {{"action", "search"}, {"query", "deploy"}}));
    EXPECT_EQ(r["count"].get<int>(), 2);
}

// ---- Helper-level tests --------------------------------------------------

TEST(SkillManagerHelpers, ValidateNameAccepts) {
    EXPECT_TRUE(validate_skill_name("hello").empty());
    EXPECT_TRUE(validate_skill_name("a1.2_3-4").empty());
}

TEST(SkillManagerHelpers, ValidateNameRejectsEmpty) {
    EXPECT_FALSE(validate_skill_name("").empty());
}

TEST(SkillManagerHelpers, ValidateNameRejectsUppercase) {
    EXPECT_FALSE(validate_skill_name("Hello").empty());
}

TEST(SkillManagerHelpers, ValidateNameRejectsLeadingDash) {
    EXPECT_FALSE(validate_skill_name("-hello").empty());
}

TEST(SkillManagerHelpers, ValidateNameRejectsTooLong) {
    std::string n(70, 'a');
    EXPECT_FALSE(validate_skill_name(n).empty());
}

TEST(SkillManagerHelpers, ValidateCategoryAcceptsEmpty) {
    EXPECT_TRUE(validate_skill_category("").empty());
}

TEST(SkillManagerHelpers, ValidateCategoryRejectsBadChars) {
    EXPECT_FALSE(validate_skill_category("My Cat").empty());
}

TEST(SkillManagerHelpers, ValidateFilePathAcceptsAllowedSubdir) {
    EXPECT_TRUE(validate_skill_file_path("scripts/run.sh").empty());
    EXPECT_TRUE(validate_skill_file_path("references/notes.md").empty());
}

TEST(SkillManagerHelpers, ValidateFilePathRejectsTraversal) {
    EXPECT_FALSE(validate_skill_file_path("../etc/passwd").empty());
    EXPECT_FALSE(validate_skill_file_path("scripts/../../boom.sh").empty());
}

TEST(SkillManagerHelpers, ValidateFilePathRejectsAbsolute) {
    EXPECT_FALSE(validate_skill_file_path("/tmp/x").empty());
}

TEST(SkillManagerHelpers, ValidateFilePathRejectsForeignSubdir) {
    EXPECT_FALSE(validate_skill_file_path("logs/x.log").empty());
}

TEST(SkillManagerHelpers, ParseFrontmatterEmpty) {
    auto fm = parse_skill_frontmatter("body without frontmatter");
    EXPECT_TRUE(fm.name.empty());
    EXPECT_TRUE(fm.tags.empty());
}

TEST(SkillManagerHelpers, ParseFrontmatterBasic) {
    auto fm = parse_skill_frontmatter(
        "---\nname: alpha\ndescription: hello world\nversion: 1.0\n---\nbody");
    EXPECT_EQ(fm.name, "alpha");
    EXPECT_EQ(fm.description, "hello world");
    EXPECT_EQ(fm.version, "1.0");
}

TEST(SkillManagerHelpers, ParseFrontmatterStripsQuotes) {
    auto fm = parse_skill_frontmatter(
        "---\nname: \"alpha\"\ndescription: 'hi'\n---\nbody");
    EXPECT_EQ(fm.name, "alpha");
    EXPECT_EQ(fm.description, "hi");
}

TEST(SkillManagerHelpers, ParseFrontmatterTags) {
    auto fm = parse_skill_frontmatter(
        "---\ntags: [a, \"b c\", d]\n---\nbody");
    ASSERT_EQ(fm.tags.size(), 3u);
    EXPECT_EQ(fm.tags[1], "b c");
}

TEST(SkillManagerHelpers, ParseFrontmatterCredentialList) {
    auto fm = parse_skill_frontmatter(
        "---\nrequired_credential_files: [google.json, twitter.json]\n---\n");
    ASSERT_EQ(fm.required_credential_files.size(), 2u);
}

TEST(SkillManagerHelpers, ParseFrontmatterUnknownGoesToRaw) {
    auto fm = parse_skill_frontmatter(
        "---\nname: x\ncustom_key: yes\n---\n");
    EXPECT_TRUE(fm.raw.contains("custom_key"));
}

TEST(SkillManagerHelpers, ParseFrontmatterIgnoresComment) {
    auto fm = parse_skill_frontmatter(
        "---\n# just a comment\nname: x\n---\n");
    EXPECT_EQ(fm.name, "x");
}

TEST(SkillManagerHelpers, EnumerateSkillsHandlesMissingDir) {
    auto v = enumerate_installed_skills("/nonexistent/path");
    EXPECT_TRUE(v.empty());
}

TEST(SkillManagerHelpers, SearchInstalledMatchesDescription) {
    InstalledSkill a;
    a.name = "alpha";
    a.description = "A friendly greeter";
    InstalledSkill b;
    b.name = "beta";
    b.description = "Database tool";
    auto matches = search_installed_skills({a, b}, "GREETER");
    ASSERT_EQ(matches.size(), 1u);
    EXPECT_EQ(matches[0].name, "alpha");
}

TEST(SkillManagerHelpers, SearchInstalledEmptyQueryReturnsEmpty) {
    InstalledSkill a;
    a.name = "alpha";
    auto matches = search_installed_skills({a}, "");
    EXPECT_TRUE(matches.empty());
}

TEST(SkillManagerHelpers, RenderInstalledListEmpty) {
    auto j = render_installed_list({});
    EXPECT_EQ(j["count"].get<int>(), 0);
    EXPECT_TRUE(j["skills"].is_array());
}

TEST(SkillManagerHelpers, AllowedSubdirsMatchSpec) {
    const auto& v = allowed_skill_subdirs();
    EXPECT_NE(std::find(v.begin(), v.end(), "scripts"), v.end());
    EXPECT_NE(std::find(v.begin(), v.end(), "templates"), v.end());
    EXPECT_NE(std::find(v.begin(), v.end(), "references"), v.end());
    EXPECT_NE(std::find(v.begin(), v.end(), "assets"), v.end());
}

}  // namespace
