#include "hermes/agent/skill_commands.hpp"

#include <gtest/gtest.h>

using hermes::agent::skill_commands::build_plan_path;
using hermes::agent::skill_commands::make_plan_slug;
using hermes::agent::skill_commands::sanitise_skill_slug;

TEST(SkillCommands, SlugFromSimpleHeading) {
    EXPECT_EQ(make_plan_slug("Fix the broken build"), "fix-the-broken-build");
}

TEST(SkillCommands, SlugCollapsesPunctuation) {
    EXPECT_EQ(make_plan_slug("Fix!!! the --- build??"), "fix-the-build");
}

TEST(SkillCommands, SlugUsesFirstLine) {
    EXPECT_EQ(make_plan_slug("Plan deployment\nlater details"),
              "plan-deployment");
}

TEST(SkillCommands, SlugEmptyFallsBack) {
    EXPECT_EQ(make_plan_slug(""), "conversation-plan");
    EXPECT_EQ(make_plan_slug("   "), "conversation-plan");
    EXPECT_EQ(make_plan_slug("!!!---???"), "conversation-plan");
}

TEST(SkillCommands, SlugCapsAt8Words) {
    auto s = make_plan_slug(
        "one two three four five six seven eight nine ten eleven");
    EXPECT_EQ(std::count(s.begin(), s.end(), '-'), 7);  // 8 words → 7 hyphens
}

TEST(SkillCommands, SlugCapsAt48Chars) {
    auto s = make_plan_slug(
        "this-is-a-very-long-heading-that-should-be-capped-at-48");
    EXPECT_LE(s.size(), 48u);
}

TEST(SkillCommands, BuildPlanPathShape) {
    auto p = build_plan_path("refactor the backend");
    EXPECT_EQ(p.extension(), ".md");
    EXPECT_NE(p.parent_path().string().find("plans"), std::string::npos);
    EXPECT_NE(p.filename().string().find("refactor-the-backend"),
              std::string::npos);
}

TEST(SkillCommands, BuildPlanPathRelative) {
    auto p = build_plan_path("x");
    // Stored relative to workspace, not absolute.
    EXPECT_FALSE(p.is_absolute());
    EXPECT_EQ(*p.begin(), std::filesystem::path(".hermes"));
}

TEST(SkillCommands, SanitiseSkillSlugStripsInvalidChars) {
    EXPECT_EQ(sanitise_skill_slug("Hello World!"), "hello-world");
    EXPECT_EQ(sanitise_skill_slug("---already---slugged---"),
              "already-slugged");
    EXPECT_EQ(sanitise_skill_slug("Skill_Name/Thing"),
              "skill-name-thing");
    EXPECT_EQ(sanitise_skill_slug(""), "");
}
