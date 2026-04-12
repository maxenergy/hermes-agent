// Gateway configuration types and loader.
#pragma once

#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

namespace hermes::gateway {

enum class Platform {
    Local,
    Telegram,
    Discord,
    WhatsApp,
    Slack,
    Signal,
    Mattermost,
    Matrix,
    HomeAssistant,
    Email,
    Sms,
    DingTalk,
    ApiServer,
    Webhook,
    Feishu,
    WeCom,
    Weixin,
    BlueBubbles
};

std::string platform_to_string(Platform p);
Platform platform_from_string(std::string_view s);

struct HomeChannel {
    Platform platform;
    std::string chat_id;
    std::string name;
};

struct SessionResetPolicy {
    std::string mode = "daily";  // daily|idle|both|none
    int at_hour = 0;             // 0-23 for daily
    int idle_minutes = 1440;     // 24h default
    bool notify = true;
    std::vector<Platform> notify_exclude;
};

struct PlatformConfig {
    bool enabled = false;
    std::string token;
    std::string api_key;
    std::optional<HomeChannel> home_channel;
    std::string reply_to_mode = "off";  // off|first|all
    nlohmann::json extra;
};

struct GatewayConfig {
    std::map<Platform, PlatformConfig> platforms;
    std::filesystem::path sessions_dir;
    SessionResetPolicy reset_policy;
    bool group_sessions_per_user = false;
    bool thread_sessions_per_user = false;
    std::string unauthorized_dm_behavior = "pair";  // pair|ignore
};

GatewayConfig load_gateway_config(const nlohmann::json& config_yaml);

}  // namespace hermes::gateway
