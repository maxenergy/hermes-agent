#include "hermes/cli/skin_engine.hpp"

#include <gtest/gtest.h>

using namespace hermes::cli;

TEST(SkinEngine, BuiltinSkinsHasFour) {
    const auto& skins = builtin_skins();
    EXPECT_EQ(skins.size(), 4u);
    EXPECT_TRUE(skins.count("default"));
    EXPECT_TRUE(skins.count("ares"));
    EXPECT_TRUE(skins.count("mono"));
    EXPECT_TRUE(skins.count("slate"));
}

TEST(SkinEngine, LoadDefault) {
    auto skin = load_skin("default");
    EXPECT_EQ(skin.name, "default");
    EXPECT_FALSE(skin.branding.agent_name.empty());
}

TEST(SkinEngine, LoadUnknownFallsBackToDefault) {
    auto skin = load_skin("nonexistent_theme_xyz");
    EXPECT_EQ(skin.name, "default");
}

TEST(SkinEngine, GetSetActiveSkin) {
    set_active_skin("ares");
    EXPECT_EQ(get_active_skin().name, "ares");
    set_active_skin("default");
    EXPECT_EQ(get_active_skin().name, "default");
}

TEST(SkinEngine, InitSkinFromConfig) {
    nlohmann::json cfg = {{"cli", {{"skin", "slate"}}}};
    init_skin_from_config(cfg);
    EXPECT_EQ(get_active_skin().name, "slate");
    // Reset.
    set_active_skin("default");
}

TEST(SkinEngine, InitSkinFromEmptyConfig) {
    nlohmann::json cfg = nlohmann::json::object();
    init_skin_from_config(cfg);
    EXPECT_EQ(get_active_skin().name, "default");
}
