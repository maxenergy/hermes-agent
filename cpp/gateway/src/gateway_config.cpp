#include <hermes/gateway/gateway_config.hpp>

#include <algorithm>
#include <stdexcept>
#include <unordered_map>

#include <hermes/core/path.hpp>

namespace hermes::gateway {

namespace {

// Bidirectional mapping tables.
struct PlatformEntry {
    Platform value;
    const char* name;
};

// Keep sorted by name for from_string binary search.
const PlatformEntry kPlatforms[] = {
    {Platform::ApiServer, "api_server"},
    {Platform::BlueBubbles, "bluebubbles"},
    {Platform::DingTalk, "dingtalk"},
    {Platform::Discord, "discord"},
    {Platform::Email, "email"},
    {Platform::Feishu, "feishu"},
    {Platform::HomeAssistant, "home_assistant"},
    {Platform::Local, "local"},
    {Platform::Matrix, "matrix"},
    {Platform::Mattermost, "mattermost"},
    {Platform::Signal, "signal"},
    {Platform::Slack, "slack"},
    {Platform::Sms, "sms"},
    {Platform::Telegram, "telegram"},
    {Platform::WeCom, "wecom"},
    {Platform::Webhook, "webhook"},
    {Platform::Weixin, "weixin"},
    {Platform::WhatsApp, "whatsapp"},
};

constexpr size_t kPlatformCount =
    sizeof(kPlatforms) / sizeof(kPlatforms[0]);

}  // namespace

std::string platform_to_string(Platform p) {
    for (size_t i = 0; i < kPlatformCount; ++i) {
        if (kPlatforms[i].value == p) {
            return kPlatforms[i].name;
        }
    }
    return "unknown";
}

Platform platform_from_string(std::string_view s) {
    // Case-insensitive lookup.
    std::string lower(s);
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) {
                       return static_cast<char>(std::tolower(c));
                   });

    for (size_t i = 0; i < kPlatformCount; ++i) {
        if (lower == kPlatforms[i].name) {
            return kPlatforms[i].value;
        }
    }
    throw std::invalid_argument(
        std::string("Unknown platform: ") + std::string(s));
}

GatewayConfig load_gateway_config(const nlohmann::json& config_yaml) {
    GatewayConfig cfg;

    // sessions_dir
    if (config_yaml.contains("sessions_dir")) {
        cfg.sessions_dir = config_yaml["sessions_dir"].get<std::string>();
    } else {
        cfg.sessions_dir =
            hermes::core::path::get_hermes_home() / "gateway" / "sessions";
    }

    // reset_policy
    if (config_yaml.contains("reset_policy")) {
        auto& rp = config_yaml["reset_policy"];
        if (rp.contains("mode"))
            cfg.reset_policy.mode = rp["mode"].get<std::string>();
        if (rp.contains("at_hour"))
            cfg.reset_policy.at_hour = rp["at_hour"].get<int>();
        if (rp.contains("idle_minutes"))
            cfg.reset_policy.idle_minutes = rp["idle_minutes"].get<int>();
        if (rp.contains("notify"))
            cfg.reset_policy.notify = rp["notify"].get<bool>();
        if (rp.contains("notify_exclude")) {
            for (auto& ex : rp["notify_exclude"]) {
                cfg.reset_policy.notify_exclude.push_back(
                    platform_from_string(ex.get<std::string>()));
            }
        }
    }

    // Scalar options
    if (config_yaml.contains("group_sessions_per_user"))
        cfg.group_sessions_per_user =
            config_yaml["group_sessions_per_user"].get<bool>();
    if (config_yaml.contains("thread_sessions_per_user"))
        cfg.thread_sessions_per_user =
            config_yaml["thread_sessions_per_user"].get<bool>();
    if (config_yaml.contains("unauthorized_dm_behavior"))
        cfg.unauthorized_dm_behavior =
            config_yaml["unauthorized_dm_behavior"].get<std::string>();

    // platforms
    if (config_yaml.contains("platforms")) {
        for (auto& [key, val] : config_yaml["platforms"].items()) {
            Platform p = platform_from_string(key);
            PlatformConfig pc;
            if (val.contains("enabled"))
                pc.enabled = val["enabled"].get<bool>();
            if (val.contains("token"))
                pc.token = val["token"].get<std::string>();
            if (val.contains("api_key"))
                pc.api_key = val["api_key"].get<std::string>();
            if (val.contains("reply_to_mode"))
                pc.reply_to_mode = val["reply_to_mode"].get<std::string>();
            if (val.contains("home_channel")) {
                auto& hc = val["home_channel"];
                HomeChannel ch;
                ch.platform = p;
                if (hc.contains("chat_id"))
                    ch.chat_id = hc["chat_id"].get<std::string>();
                if (hc.contains("name"))
                    ch.name = hc["name"].get<std::string>();
                pc.home_channel = ch;
            }
            if (val.contains("extra"))
                pc.extra = val["extra"];
            cfg.platforms[p] = std::move(pc);
        }
    }

    return cfg;
}

}  // namespace hermes::gateway
