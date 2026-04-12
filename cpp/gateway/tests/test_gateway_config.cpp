#include <gtest/gtest.h>

#include <hermes/gateway/gateway_config.hpp>

namespace hg = hermes::gateway;

TEST(GatewayConfig, PlatformEnumRoundTrip) {
    // Every platform should survive to_string -> from_string.
    std::vector<hg::Platform> all = {
        hg::Platform::Local,         hg::Platform::Telegram,
        hg::Platform::Discord,       hg::Platform::WhatsApp,
        hg::Platform::Slack,         hg::Platform::Signal,
        hg::Platform::Mattermost,    hg::Platform::Matrix,
        hg::Platform::HomeAssistant, hg::Platform::Email,
        hg::Platform::Sms,           hg::Platform::DingTalk,
        hg::Platform::ApiServer,     hg::Platform::Webhook,
        hg::Platform::Feishu,        hg::Platform::WeCom,
        hg::Platform::Weixin,        hg::Platform::BlueBubbles,
    };

    for (auto p : all) {
        auto s = hg::platform_to_string(p);
        auto p2 = hg::platform_from_string(s);
        EXPECT_EQ(p, p2) << "Failed round-trip for " << s;
    }
}

TEST(GatewayConfig, PlatformFromStringCaseInsensitive) {
    EXPECT_EQ(hg::Platform::Telegram,
              hg::platform_from_string("TELEGRAM"));
    EXPECT_EQ(hg::Platform::Discord,
              hg::platform_from_string("Discord"));
}

TEST(GatewayConfig, PlatformFromStringUnknownThrows) {
    EXPECT_THROW(hg::platform_from_string("nonexistent"),
                 std::invalid_argument);
}

TEST(GatewayConfig, LoadFromJson) {
    nlohmann::json j = {
        {"sessions_dir", "/tmp/test-sessions"},
        {"group_sessions_per_user", true},
        {"unauthorized_dm_behavior", "ignore"},
        {"reset_policy",
         {{"mode", "idle"}, {"idle_minutes", 60}, {"notify", false}}},
        {"platforms",
         {{"telegram", {{"enabled", true}, {"token", "abc123"}}}}},
    };

    auto cfg = hg::load_gateway_config(j);
    EXPECT_EQ(cfg.sessions_dir, "/tmp/test-sessions");
    EXPECT_TRUE(cfg.group_sessions_per_user);
    EXPECT_EQ(cfg.unauthorized_dm_behavior, "ignore");
    EXPECT_EQ(cfg.reset_policy.mode, "idle");
    EXPECT_EQ(cfg.reset_policy.idle_minutes, 60);
    EXPECT_FALSE(cfg.reset_policy.notify);
    ASSERT_EQ(cfg.platforms.count(hg::Platform::Telegram), 1u);
    EXPECT_TRUE(cfg.platforms.at(hg::Platform::Telegram).enabled);
    EXPECT_EQ(cfg.platforms.at(hg::Platform::Telegram).token, "abc123");
}

TEST(GatewayConfig, ResetPolicyDefaults) {
    nlohmann::json j = nlohmann::json::object();
    auto cfg = hg::load_gateway_config(j);
    EXPECT_EQ(cfg.reset_policy.mode, "daily");
    EXPECT_EQ(cfg.reset_policy.at_hour, 0);
    EXPECT_EQ(cfg.reset_policy.idle_minutes, 1440);
    EXPECT_TRUE(cfg.reset_policy.notify);
    EXPECT_EQ(cfg.unauthorized_dm_behavior, "pair");
}
