#include "hermes/skills/skills_hub.hpp"

#include <gtest/gtest.h>

using namespace hermes::skills;

// When no HTTP transport is available (test environment), the hub methods
// should degrade gracefully.

TEST(SkillsHubTest, SearchReturnsEmptyWithoutTransport) {
    SkillsHub hub;
    auto results = hub.search("anything");
    EXPECT_TRUE(results.empty());
}

TEST(SkillsHubTest, InstallReturnsFalseWithoutTransport) {
    SkillsHub hub;
    EXPECT_FALSE(hub.install("some-skill"));
}

TEST(SkillsHubTest, GetReturnsNulloptWithoutTransport) {
    SkillsHub hub;
    EXPECT_FALSE(hub.get("whatever").has_value());
}

TEST(SkillsHubTest, UninstallReturnsFalseForNonexistent) {
    SkillsHub hub;
    EXPECT_FALSE(hub.uninstall("nonexistent-skill-xyz"));
}

TEST(SkillsHubTest, UpdateReturnsFalseWithoutTransport) {
    SkillsHub hub;
    EXPECT_FALSE(hub.update("some-skill"));
}
