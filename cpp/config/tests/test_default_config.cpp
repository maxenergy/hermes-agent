#include "hermes/config/default_config.hpp"
#include "hermes/config/optional_env_vars.hpp"

#include <gtest/gtest.h>

namespace hc = hermes::config;

TEST(DefaultConfig, HasVersionStamp) {
    const auto& cfg = hc::default_config();
    ASSERT_TRUE(cfg.contains("_config_version"));
    EXPECT_EQ(cfg["_config_version"].get<int>(), hc::kCurrentConfigVersion);
    EXPECT_EQ(hc::kCurrentConfigVersion, 5);
}

TEST(DefaultConfig, HasModelKey) {
    const auto& cfg = hc::default_config();
    ASSERT_TRUE(cfg.contains("model"));
    EXPECT_TRUE(cfg["model"].is_string());
}

TEST(DefaultConfig, TerminalBackendIsLocal) {
    const auto& cfg = hc::default_config();
    ASSERT_TRUE(cfg.contains("terminal"));
    ASSERT_TRUE(cfg["terminal"].is_object());
    ASSERT_TRUE(cfg["terminal"].contains("backend"));
    EXPECT_EQ(cfg["terminal"]["backend"].get<std::string>(), "local");
}

TEST(DefaultConfig, ContainsScaffoldDicts) {
    const auto& cfg = hc::default_config();
    EXPECT_TRUE(cfg.contains("tools"));
    EXPECT_TRUE(cfg.contains("display"));
    EXPECT_TRUE(cfg.contains("memory"));
    EXPECT_TRUE(cfg.contains("messaging"));
    EXPECT_TRUE(cfg.contains("web"));
    EXPECT_TRUE(cfg.contains("tts"));
}

TEST(OptionalEnvVars, HasKnownProviderKeys) {
    const auto& vars = hc::optional_env_vars();
    // Spot-check that exact Python names were ported, not invented.
    EXPECT_TRUE(vars.count("OPENROUTER_API_KEY") == 1);
    EXPECT_TRUE(vars.count("ANTHROPIC_API_KEY") == 1);
    EXPECT_TRUE(vars.count("GEMINI_API_KEY") == 1);
    EXPECT_TRUE(vars.count("TELEGRAM_BOT_TOKEN") == 1);
    EXPECT_TRUE(vars.count("MATRIX_HOMESERVER") == 1);
    EXPECT_TRUE(vars.count("FIRECRAWL_API_KEY") == 1);
    EXPECT_TRUE(vars.count("ELEVENLABS_API_KEY") == 1);
    EXPECT_GE(vars.size(), 20u);
}

TEST(OptionalEnvVars, PasswordFlagSetForSecrets) {
    const auto& vars = hc::optional_env_vars();
    EXPECT_TRUE(vars.at("OPENROUTER_API_KEY").password);
    EXPECT_FALSE(vars.at("NOUS_BASE_URL").password);
}
