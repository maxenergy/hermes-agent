#include "hermes/skills/skills_hub.hpp"

#include <gtest/gtest.h>

using namespace hermes::skills;

// 1. search returns empty (stub)
TEST(SkillsHubTest, SearchReturnsEmpty) {
    SkillsHub hub;
    auto results = hub.search("anything");
    EXPECT_TRUE(results.empty());
}

// 2. install returns false (stub)
TEST(SkillsHubTest, InstallReturnsFalse) {
    SkillsHub hub;
    EXPECT_FALSE(hub.install("some-skill"));
}

// 3. get returns nullopt (stub)
TEST(SkillsHubTest, GetReturnsNullopt) {
    SkillsHub hub;
    EXPECT_FALSE(hub.get("whatever").has_value());
}
