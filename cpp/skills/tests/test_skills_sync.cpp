#include "hermes/skills/skills_sync.hpp"

#include "hermes/skills/skills_hub.hpp"

#include <gtest/gtest.h>

using hermes::skills::SkillsHub;
using hermes::skills::SkillsSync;

TEST(SkillsSync, PushWithoutTokenProducesError) {
    SkillsHub hub;
    SkillsSync sync(&hub);
    auto r = sync.push("");
    EXPECT_EQ(r.uploaded, 0);
    EXPECT_FALSE(r.errors.empty());
    EXPECT_NE(r.errors.front().find("auth token"), std::string::npos);
}

TEST(SkillsSync, PullWithoutHubProducesError) {
    SkillsSync sync(nullptr);
    auto r = sync.pull();
    EXPECT_EQ(r.downloaded, 0);
    ASSERT_FALSE(r.errors.empty());
    EXPECT_NE(r.errors.front().find("backend"), std::string::npos);
}

TEST(SkillsSync, PullWithStubHubReturnsZeroCounters) {
    SkillsHub hub;  // stub returns nullopt on get()
    SkillsSync sync(&hub);
    auto r = sync.pull();
    EXPECT_EQ(r.downloaded, 0);
    EXPECT_EQ(r.uploaded, 0);
}

TEST(SkillsSync, SyncCombinesPullAndPushErrors) {
    SkillsHub hub;
    SkillsSync sync(&hub);
    auto r = sync.sync("");  // empty token triggers push error
    EXPECT_FALSE(r.errors.empty());
}
