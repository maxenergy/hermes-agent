// Phase 12 — Platform adapter factory implementation.
#include "adapter_factory.hpp"

#include "api_server.hpp"
#include "bluebubbles.hpp"
#include "dingtalk.hpp"
#include "discord.hpp"
#include "email.hpp"
#include "feishu.hpp"
#include "home_assistant.hpp"
#include "local.hpp"
#include "matrix.hpp"
#include "mattermost.hpp"
#include "signal.hpp"
#include "slack.hpp"
#include "sms.hpp"
#include "telegram.hpp"
#include "webhook.hpp"
#include "wecom.hpp"
#include "weixin.hpp"
#include "whatsapp.hpp"

namespace hermes::gateway::platforms {

namespace {

// Helper: extract a string from PlatformConfig.extra JSON, or "".
std::string extra_str(const PlatformConfig& cfg, const std::string& key) {
    if (cfg.extra.contains(key) && cfg.extra[key].is_string())
        return cfg.extra[key].get<std::string>();
    return {};
}

}  // namespace

std::unique_ptr<BasePlatformAdapter> create_adapter(Platform p,
                                                     const PlatformConfig& cfg) {
    switch (p) {
        case Platform::Telegram: {
            TelegramAdapter::Config tc;
            tc.bot_token = cfg.token;
            tc.reply_to_mode = cfg.reply_to_mode;
            tc.webhook_url = extra_str(cfg, "webhook_url");
            tc.use_webhook = !tc.webhook_url.empty();
            return std::make_unique<TelegramAdapter>(std::move(tc));
        }
        case Platform::Discord: {
            DiscordAdapter::Config dc;
            dc.bot_token = cfg.token;
            dc.application_id = extra_str(cfg, "application_id");
            return std::make_unique<DiscordAdapter>(std::move(dc));
        }
        case Platform::Slack: {
            SlackAdapter::Config sc;
            sc.bot_token = cfg.token;
            sc.signing_secret = extra_str(cfg, "signing_secret");
            sc.app_token = extra_str(cfg, "app_token");
            return std::make_unique<SlackAdapter>(std::move(sc));
        }
        case Platform::WhatsApp: {
            WhatsAppAdapter::Config wc;
            wc.session_dir = extra_str(cfg, "session_dir");
            wc.phone = extra_str(cfg, "phone");
            return std::make_unique<WhatsAppAdapter>(std::move(wc));
        }
        case Platform::Signal: {
            SignalAdapter::Config sc;
            sc.http_url = extra_str(cfg, "http_url");
            sc.account = extra_str(cfg, "account");
            return std::make_unique<SignalAdapter>(std::move(sc));
        }
        case Platform::Matrix: {
            MatrixAdapter::Config mc;
            mc.homeserver = extra_str(cfg, "homeserver");
            mc.username = extra_str(cfg, "username");
            mc.password = extra_str(cfg, "password");
            mc.access_token = cfg.token;
            return std::make_unique<MatrixAdapter>(std::move(mc));
        }
        case Platform::Mattermost: {
            MattermostAdapter::Config mc;
            mc.token = cfg.token;
            mc.url = extra_str(cfg, "url");
            return std::make_unique<MattermostAdapter>(std::move(mc));
        }
        case Platform::Email: {
            EmailAdapter::Config ec;
            ec.address = extra_str(cfg, "address");
            ec.password = extra_str(cfg, "password");
            ec.imap_host = extra_str(cfg, "imap_host");
            ec.smtp_host = extra_str(cfg, "smtp_host");
            return std::make_unique<EmailAdapter>(std::move(ec));
        }
        case Platform::Sms: {
            SmsAdapter::Config sc;
            sc.twilio_account_sid = extra_str(cfg, "twilio_account_sid");
            sc.twilio_auth_token = extra_str(cfg, "twilio_auth_token");
            sc.from_number = extra_str(cfg, "from_number");
            return std::make_unique<SmsAdapter>(std::move(sc));
        }
        case Platform::DingTalk: {
            DingTalkAdapter::Config dc;
            dc.client_id = extra_str(cfg, "client_id");
            dc.client_secret = extra_str(cfg, "client_secret");
            return std::make_unique<DingTalkAdapter>(std::move(dc));
        }
        case Platform::Feishu: {
            FeishuAdapter::Config fc;
            fc.app_id = extra_str(cfg, "app_id");
            fc.app_secret = extra_str(cfg, "app_secret");
            return std::make_unique<FeishuAdapter>(std::move(fc));
        }
        case Platform::WeCom: {
            WeComAdapter::Config wc;
            wc.bot_id = extra_str(cfg, "bot_id");
            wc.message_token = extra_str(cfg, "message_token");
            wc.webhook_url = extra_str(cfg, "webhook_url");
            return std::make_unique<WeComAdapter>(std::move(wc));
        }
        case Platform::Weixin: {
            WeixinAdapter::Config wc;
            wc.appid = extra_str(cfg, "appid");
            wc.appsecret = extra_str(cfg, "appsecret");
            wc.token = cfg.token;
            return std::make_unique<WeixinAdapter>(std::move(wc));
        }
        case Platform::BlueBubbles: {
            BlueBubblesAdapter::Config bc;
            bc.server_url = extra_str(cfg, "server_url");
            bc.password = extra_str(cfg, "password");
            return std::make_unique<BlueBubblesAdapter>(std::move(bc));
        }
        case Platform::HomeAssistant: {
            HomeAssistantAdapter::Config hc;
            hc.hass_token = cfg.token;
            hc.hass_url = extra_str(cfg, "hass_url");
            if (hc.hass_url.empty()) hc.hass_url = "http://localhost:8123";
            return std::make_unique<HomeAssistantAdapter>(std::move(hc));
        }
        case Platform::ApiServer: {
            ApiServerAdapter::Config ac;
            ac.hmac_secret = extra_str(cfg, "hmac_secret");
            return std::make_unique<ApiServerAdapter>(std::move(ac));
        }
        case Platform::Webhook: {
            WebhookAdapter::Config wc;
            wc.signature_secret = extra_str(cfg, "signature_secret");
            wc.endpoint_url = extra_str(cfg, "endpoint_url");
            return std::make_unique<WebhookAdapter>(std::move(wc));
        }
        case Platform::Local:
        default:
            return std::make_unique<LocalAdapter>();
    }
}

std::vector<Platform> available_adapters() {
    return {
        Platform::Local,
        Platform::Telegram,
        Platform::Discord,
        Platform::Slack,
        Platform::WhatsApp,
        Platform::Signal,
        Platform::Matrix,
        Platform::Mattermost,
        Platform::Email,
        Platform::Sms,
        Platform::DingTalk,
        Platform::Feishu,
        Platform::WeCom,
        Platform::Weixin,
        Platform::BlueBubbles,
        Platform::HomeAssistant,
        Platform::ApiServer,
        Platform::Webhook,
    };
}

}  // namespace hermes::gateway::platforms
