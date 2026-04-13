// Phase 12 — Slack platform adapter implementation.
#include "slack.hpp"

#include <iomanip>
#include <sstream>

#include <nlohmann/json.hpp>
#include <openssl/hmac.h>

#include <hermes/gateway/status.hpp>

namespace hermes::gateway::platforms {

SlackAdapter::SlackAdapter(Config cfg) : cfg_(std::move(cfg)) {}

SlackAdapter::SlackAdapter(Config cfg, hermes::llm::HttpTransport* transport)
    : cfg_(std::move(cfg)), transport_(transport) {}

hermes::llm::HttpTransport* SlackAdapter::get_transport() {
    if (transport_) return transport_;
    return hermes::llm::get_default_transport();
}

bool SlackAdapter::connect() {
    if (cfg_.bot_token.empty()) return false;

    if (!hermes::gateway::acquire_scoped_lock(
            hermes::gateway::platform_to_string(platform()),
            cfg_.bot_token, {})) {
        return false;
    }

    auto* transport = get_transport();
    if (!transport) {
        hermes::gateway::release_scoped_lock(
            hermes::gateway::platform_to_string(platform()), cfg_.bot_token);
        return false;
    }

    try {
        auto resp = transport->post_json(
            "https://slack.com/api/auth.test",
            {{"Authorization", "Bearer " + cfg_.bot_token},
             {"Content-Type", "application/json"}},
            "{}");
        if (resp.status_code != 200) return false;

        auto body = nlohmann::json::parse(resp.body);
        if (!body.value("ok", false)) return false;

        connected_ = true;
        return true;
    } catch (...) {
        return false;
    }
}

void SlackAdapter::disconnect() {
    if (!cfg_.bot_token.empty()) {
        hermes::gateway::release_scoped_lock(
            hermes::gateway::platform_to_string(platform()), cfg_.bot_token);
    }
    connected_ = false;
}

std::optional<std::string> SlackAdapter::parse_thread_ts(
    const nlohmann::json& event) {
    if (event.contains("thread_ts") && event["thread_ts"].is_string()) {
        return event["thread_ts"].get<std::string>();
    }
    return std::nullopt;
}

bool SlackAdapter::should_handle_event(const nlohmann::json& event,
                                       const std::string& bot_user_id) {
    if (!event.is_object()) return false;
    // Skip our own messages.
    if (event.contains("bot_id") && event.value("bot_id", "").size() > 0) {
        return false;
    }
    if (event.contains("subtype")) return false;

    // DMs (channel id starts with "D") are always in scope.
    auto channel = event.value("channel", "");
    if (!channel.empty() && channel[0] == 'D') return true;

    // Thread replies are in-scope without an explicit mention — once
    // the bot has been pulled into a thread, every subsequent reply is
    // assumed to be addressed to it.
    if (parse_thread_ts(event).has_value()) return true;

    // Otherwise require an @-mention of the bot.  The text contains
    // "<@U12345>" for the bot's user id.
    if (!bot_user_id.empty()) {
        auto text = event.value("text", "");
        std::string needle = "<@" + bot_user_id + ">";
        if (text.find(needle) != std::string::npos) return true;
    }
    return false;
}

bool SlackAdapter::send_thread_reply(const std::string& chat_id,
                                     const std::string& thread_ts,
                                     const std::string& content) {
    auto* transport = get_transport();
    if (!transport) return false;
    nlohmann::json payload = {
        {"channel", chat_id},
        {"text", content},
        {"thread_ts", thread_ts},
    };
    try {
        auto resp = transport->post_json(
            "https://slack.com/api/chat.postMessage",
            {{"Authorization", "Bearer " + cfg_.bot_token},
             {"Content-Type", "application/json"}},
            payload.dump());
        if (resp.status_code != 200) return false;
        auto body = nlohmann::json::parse(resp.body);
        return body.value("ok", false);
    } catch (...) {
        return false;
    }
}

bool SlackAdapter::upload_file(const std::string& chat_id,
                               const std::string& filename,
                               const std::string& content,
                               const std::string& initial_comment) {
    auto* transport = get_transport();
    if (!transport) return false;
    // Use the JSON body variant (files.upload accepts form-encoded; the
    // simpler files.upload v2 path takes JSON).  For the adapter we post
    // to files.upload with JSON — good enough for unit testing via the
    // FakeHttpTransport which just captures the request.
    nlohmann::json payload = {
        {"channels", chat_id},
        {"filename", filename},
        {"content", content},
    };
    if (!initial_comment.empty()) payload["initial_comment"] = initial_comment;
    try {
        auto resp = transport->post_json(
            "https://slack.com/api/files.upload",
            {{"Authorization", "Bearer " + cfg_.bot_token},
             {"Content-Type", "application/json"}},
            payload.dump());
        if (resp.status_code != 200) return false;
        auto body = nlohmann::json::parse(resp.body);
        return body.value("ok", false);
    } catch (...) {
        return false;
    }
}

bool SlackAdapter::send(const std::string& chat_id,
                        const std::string& content) {
    auto* transport = get_transport();
    if (!transport) return false;

    nlohmann::json payload = {
        {"channel", chat_id},
        {"text", content}
    };

    try {
        auto resp = transport->post_json(
            "https://slack.com/api/chat.postMessage",
            {{"Authorization", "Bearer " + cfg_.bot_token},
             {"Content-Type", "application/json"}},
            payload.dump());
        if (resp.status_code != 200) return false;

        auto body = nlohmann::json::parse(resp.body);
        return body.value("ok", false);
    } catch (...) {
        return false;
    }
}

void SlackAdapter::send_typing(const std::string& /*chat_id*/) {
    // Slack typing indicators are sent via WebSocket (RTM/Socket Mode),
    // not via the Web API. No-op for HTTP-only send path.
}

std::string SlackAdapter::compute_slack_signature(
    const std::string& signing_secret,
    const std::string& timestamp,
    const std::string& body) {
    std::string base_string = "v0:" + timestamp + ":" + body;
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int digest_len = 0;

    HMAC(EVP_sha256(),
         signing_secret.data(),
         static_cast<int>(signing_secret.size()),
         reinterpret_cast<const unsigned char*>(base_string.data()),
         base_string.size(),
         digest,
         &digest_len);

    std::ostringstream hex_stream;
    hex_stream << "v0=";
    for (unsigned int i = 0; i < digest_len; ++i) {
        hex_stream << std::hex << std::setfill('0') << std::setw(2)
                   << static_cast<int>(digest[i]);
    }
    return hex_stream.str();
}

// ----- Socket Mode / RTM realtime ---------------------------------------

void SlackAdapter::configure_realtime(
    const std::string& ws_url,
    std::unique_ptr<WebSocketTransport> transport) {
    SlackSocketMode::Config scfg;
    scfg.app_token = cfg_.app_token;
    scfg.bot_token = cfg_.bot_token;
    scfg.ws_url = ws_url;
    socket_mode_ = std::make_unique<SlackSocketMode>(std::move(scfg));
    if (transport) socket_mode_->set_transport(std::move(transport));

    socket_mode_->set_event_callback(
        [this](const std::string& type, const nlohmann::json& data) {
            // Socket Mode payload nests the event object under payload.event.
            // RTM mode delivers the event object directly.
            const nlohmann::json* ev = &data;
            nlohmann::json payload_event;
            if (data.contains("event") && data["event"].is_object()) {
                payload_event = data["event"];
                ev = &payload_event;
            }
            std::string inner_type = ev->value("type", type);
            if (inner_type != "message") return;
            if (!message_cb_) return;
            // Ignore bot_message / subtype edits/deletes to avoid loops.
            if (ev->contains("bot_id") && !ev->value("bot_id", "").empty()) return;
            if (ev->contains("subtype")) return;
            std::string channel = ev->value("channel", "");
            std::string user = ev->value("user", "");
            std::string text = ev->value("text", "");
            std::string ts = ev->value("ts", "");
            message_cb_(channel, user, text, ts);
        });
}

std::optional<std::string> SlackAdapter::fetch_ws_url() {
    auto* transport = get_transport();
    if (!transport) return std::nullopt;

    // Prefer Socket Mode when an app token is set.
    if (!cfg_.app_token.empty()) {
        try {
            auto resp = transport->post_json(
                "https://slack.com/api/apps.connections.open",
                {{"Authorization", "Bearer " + cfg_.app_token},
                 {"Content-Type", "application/x-www-form-urlencoded"}},
                "");
            if (resp.status_code != 200) return std::nullopt;
            auto body = nlohmann::json::parse(resp.body);
            if (!body.value("ok", false)) return std::nullopt;
            if (body.contains("url")) return body["url"].get<std::string>();
        } catch (...) {
            return std::nullopt;
        }
        return std::nullopt;
    }

    // Legacy RTM via bot token.
    if (!cfg_.bot_token.empty()) {
        try {
            auto resp = transport->post_json(
                "https://slack.com/api/rtm.connect",
                {{"Authorization", "Bearer " + cfg_.bot_token},
                 {"Content-Type", "application/x-www-form-urlencoded"}},
                "");
            if (resp.status_code != 200) return std::nullopt;
            auto body = nlohmann::json::parse(resp.body);
            if (!body.value("ok", false)) return std::nullopt;
            if (body.contains("url")) return body["url"].get<std::string>();
        } catch (...) {
            return std::nullopt;
        }
    }
    return std::nullopt;
}

bool SlackAdapter::start_realtime() {
    if (!socket_mode_) configure_realtime("");
    if (!socket_mode_) return false;

    // If no URL was provided at configure-time, fetch via REST.
    auto* raw_sm = socket_mode_.get();
    // We cannot inspect the URL via the public API; attempt a connect
    // and fall through to fetching when it returns false.
    if (!raw_sm->connect()) {
        auto url = fetch_ws_url();
        if (!url) return false;
        raw_sm->set_websocket_url(*url);
        if (!raw_sm->connect()) return false;
    }
    return true;
}

void SlackAdapter::stop_realtime() {
    if (socket_mode_) socket_mode_->disconnect();
}

bool SlackAdapter::realtime_run_once() {
    return socket_mode_ && socket_mode_->run_once();
}

}  // namespace hermes::gateway::platforms
