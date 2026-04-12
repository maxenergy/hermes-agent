#include "hermes/skills/skill_commands.hpp"
#include "hermes/skills/skill_utils.hpp"

#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <regex>

namespace fs = std::filesystem;
using namespace hermes::skills;

class TmpDir {
public:
    TmpDir() {
        char tpl[] = "/tmp/hermes_skills_cmd_XXXXXX";
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

// 1. load_skill_payload from mock skill
TEST(SkillCommandsTest, LoadSkillPayloadFromMock) {
    TmpDir tmp;
    auto skill_dir = tmp.path() / "skills" / "test-cmd";
    fs::create_directories(skill_dir);
    {
        std::ofstream ofs(skill_dir / "SKILL.md");
        ofs << "---\nname: test-cmd\ndescription: Test command\n---\n"
            << "This is the body content.";
    }

    setenv("HERMES_HOME", tmp.path().c_str(), 1);
    auto payload = load_skill_payload("test-cmd");
    unsetenv("HERMES_HOME");

    ASSERT_TRUE(payload.has_value());
    EXPECT_EQ(payload->name, "test-cmd");
    EXPECT_EQ(payload->content, "This is the body content.");
    EXPECT_TRUE(payload->metadata.contains("description"));
}

// 2. load_skill_payload returns nullopt for missing skill
TEST(SkillCommandsTest, LoadSkillPayloadMissing) {
    TmpDir tmp;
    fs::create_directories(tmp.path() / "skills");

    setenv("HERMES_HOME", tmp.path().c_str(), 1);
    auto payload = load_skill_payload("nonexistent");
    unsetenv("HERMES_HOME");

    EXPECT_FALSE(payload.has_value());
}

// 3. build_plan_path format: YYYYMMDD-HHMMSS-slug.md
TEST(SkillCommandsTest, BuildPlanPathFormat) {
    auto path = build_plan_path("my-plan");
    auto filename = path.filename().string();

    // Should match YYYYMMDD-HHMMSS-my-plan.md
    std::regex pattern(R"(\d{8}-\d{6}-my-plan\.md)");
    EXPECT_TRUE(std::regex_match(filename, pattern))
        << "Unexpected filename: " << filename;

    // Should be under .hermes/plans/
    EXPECT_NE(path.string().find("plans"), std::string::npos);
}

// 4. builtin_skills contains /plan
TEST(SkillCommandsTest, BuiltinSkillsContainsPlan) {
    const auto& skills = builtin_skills();
    ASSERT_GE(skills.size(), 3u);

    bool found_plan = false;
    bool found_debug = false;
    bool found_web = false;
    for (const auto& s : skills) {
        if (s.name == "/plan") found_plan = true;
        if (s.name == "/debug") found_debug = true;
        if (s.name == "/web-research") found_web = true;
    }
    EXPECT_TRUE(found_plan);
    EXPECT_TRUE(found_debug);
    EXPECT_TRUE(found_web);
}

// 5. Frontmatter stripped from payload
TEST(SkillCommandsTest, FrontmatterStrippedFromPayload) {
    TmpDir tmp;
    auto skill_dir = tmp.path() / "skills" / "strip-test";
    fs::create_directories(skill_dir);
    {
        std::ofstream ofs(skill_dir / "SKILL.md");
        ofs << "---\nkey: value\n---\nClean body only.";
    }

    setenv("HERMES_HOME", tmp.path().c_str(), 1);
    auto payload = load_skill_payload("strip-test");
    unsetenv("HERMES_HOME");

    ASSERT_TRUE(payload.has_value());
    // Body must not contain frontmatter fences.
    EXPECT_EQ(payload->content.find("---"), std::string::npos);
    EXPECT_EQ(payload->content, "Clean body only.");
}
