#include "hermes/skills/skill_utils.hpp"

#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;
using namespace hermes::skills;

// Helper: create a temp dir that is cleaned up on scope exit.
class TmpDir {
public:
    TmpDir() {
        char tpl[] = "/tmp/hermes_skills_test_XXXXXX";
        path_ = fs::path(mkdtemp(tpl));
    }
    ~TmpDir() {
        std::error_code ec;
        fs::remove_all(path_, ec);
    }
    const fs::path& path() const { return path_; }
private:
    fs::path path_;
};

// 1. get_all_skills_dirs returns paths under HERMES_HOME
TEST(SkillUtilsTest, GetAllSkillsDirsReturnsPaths) {
    TmpDir tmp;
    // Create the expected subdirectories.
    fs::create_directories(tmp.path() / "skills");
    fs::create_directories(tmp.path() / "optional-skills");
    fs::create_directories(tmp.path() / "installed-skills");

    // Override HERMES_HOME.
    setenv("HERMES_HOME", tmp.path().c_str(), 1);
    auto dirs = get_all_skills_dirs();
    unsetenv("HERMES_HOME");

    ASSERT_GE(dirs.size(), 3u);
    // All should be children of tmp.
    for (const auto& d : dirs) {
        EXPECT_TRUE(d.string().find(tmp.path().string()) == 0);
    }
}

// 2. parse_frontmatter extracts YAML
TEST(SkillUtilsTest, ParseFrontmatterExtractsYAML) {
    std::string md = "---\nname: test-skill\ndescription: A test\n---\nBody here.";
    auto [meta, body] = parse_frontmatter(md);

    ASSERT_FALSE(meta.is_null());
    EXPECT_EQ(meta["name"].get<std::string>(), "test-skill");
    EXPECT_EQ(meta["description"].get<std::string>(), "A test");
    EXPECT_EQ(body, "Body here.");
}

// 3. parse_frontmatter with no frontmatter returns null + full content
TEST(SkillUtilsTest, ParseFrontmatterNoFence) {
    std::string md = "Just some text without frontmatter.";
    auto [meta, body] = parse_frontmatter(md);

    EXPECT_TRUE(meta.is_null());
    EXPECT_EQ(body, md);
}

// 4. skill_matches_platform filters correctly
TEST(SkillUtilsTest, SkillMatchesPlatformFilters) {
    SkillMetadata s;
    s.platforms = {"cli", "telegram"};

    EXPECT_TRUE(skill_matches_platform(s, "cli"));
    EXPECT_TRUE(skill_matches_platform(s, "telegram"));
    EXPECT_FALSE(skill_matches_platform(s, "discord"));

    // Empty platforms means "all platforms".
    SkillMetadata s2;
    EXPECT_TRUE(skill_matches_platform(s2, "anything"));
}

// 5. get_disabled_skill_names from config
TEST(SkillUtilsTest, GetDisabledSkillNames) {
    nlohmann::json config = {
        {"disabled_skills", {"alpha", "beta"}}
    };
    auto names = get_disabled_skill_names(config);
    ASSERT_EQ(names.size(), 2u);
    EXPECT_EQ(names[0], "alpha");
    EXPECT_EQ(names[1], "beta");

    // Empty config returns empty.
    EXPECT_TRUE(get_disabled_skill_names(nlohmann::json::object()).empty());
    EXPECT_TRUE(get_disabled_skill_names(nlohmann::json(42)).empty());
}

// 6. Empty dir produces empty iter_skill_index
TEST(SkillUtilsTest, EmptyDirEmptyIter) {
    TmpDir tmp;
    fs::create_directories(tmp.path() / "skills");

    setenv("HERMES_HOME", tmp.path().c_str(), 1);
    auto skills = iter_skill_index();
    unsetenv("HERMES_HOME");

    EXPECT_TRUE(skills.empty());
}

// 7. Create mock skill -> found by iter_skill_index
TEST(SkillUtilsTest, MockSkillFound) {
    TmpDir tmp;
    auto skill_dir = tmp.path() / "skills" / "my-skill";
    fs::create_directories(skill_dir);

    // Write a SKILL.md with frontmatter.
    {
        std::ofstream ofs(skill_dir / "SKILL.md");
        ofs << "---\n"
            << "description: My skill does things\n"
            << "version: 1.0.0\n"
            << "platforms: cli, telegram\n"
            << "---\n"
            << "# My Skill\nContent here.\n";
    }

    setenv("HERMES_HOME", tmp.path().c_str(), 1);
    auto skills = iter_skill_index();
    unsetenv("HERMES_HOME");

    ASSERT_EQ(skills.size(), 1u);
    EXPECT_EQ(skills[0].name, "my-skill");
    EXPECT_EQ(skills[0].description, "My skill does things");
    EXPECT_EQ(skills[0].version, "1.0.0");
    ASSERT_EQ(skills[0].platforms.size(), 2u);
    EXPECT_EQ(skills[0].platforms[0], "cli");
    EXPECT_EQ(skills[0].platforms[1], "telegram");
}

// 8. extract_skill_conditions
TEST(SkillUtilsTest, ExtractSkillConditions) {
    nlohmann::json fm = {{"conditions", {"has-git", "has-node"}}};
    auto conds = extract_skill_conditions(fm);
    ASSERT_EQ(conds.size(), 2u);
    EXPECT_EQ(conds[0], "has-git");

    // Null frontmatter returns empty.
    EXPECT_TRUE(extract_skill_conditions(nlohmann::json(nullptr)).empty());
}
