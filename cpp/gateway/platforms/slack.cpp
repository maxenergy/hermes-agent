// Phase 12 — Slack platform adapter implementation.
//
// Expanded to cover the Web API surface used by gateway/platforms/slack.py:
// chat.* messaging with Block Kit, conversations.*, users.*, reactions.*,
// pins.*, stars.*, files.* (inline + upload_v2), views.* (open/push/update/
// publish), mrkdwn formatting, Events-API signature verification, Enterprise
// Grid identity extraction, and 429 Retry-After-aware request retry.
#include "slack.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <iomanip>
#include <regex>
#include <sstream>
#include <thread>
#include <unordered_set>

#include <nlohmann/json.hpp>
#include <openssl/hmac.h>

#include <hermes/gateway/status.hpp>

namespace hermes::gateway::platforms {

namespace {

constexpr const char* kSlackApi = "https://slack.com/api/";

std::string to_hex(const unsigned char* data, std::size_t len) {
    std::ostringstream os;
    for (std::size_t i = 0; i < len; ++i) {
        os << std::hex << std::setfill('0') << std::setw(2)
           << static_cast<int>(data[i]);
    }
    return os.str();
}

bool constant_time_equals(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    unsigned char diff = 0;
    for (std::size_t i = 0; i < a.size(); ++i) {
        diff |= static_cast<unsigned char>(a[i]) ^
                static_cast<unsigned char>(b[i]);
    }
    return diff == 0;
}

std::int64_t parse_retry_after(
    const std::unordered_map<std::string, std::string>& headers) {
    // Headers may be case-insensitive — scan for Retry-After / retry-after.
    for (const auto& [k, v] : headers) {
        std::string lower;
        lower.reserve(k.size());
        for (char c : k) lower.push_back(std::tolower(c));
        if (lower == "retry-after") {
            try {
                return std::stoll(v) * 1000;  // seconds → ms
            } catch (...) {
                return 1000;
            }
        }
    }
    return 1000;
}

}  // namespace

SlackAdapter::SlackAdapter(Config cfg) : cfg_(std::move(cfg)) {}

SlackAdapter::SlackAdapter(Config cfg, hermes::llm::HttpTransport* transport)
    : cfg_(std::move(cfg)), transport_(transport) {}

hermes::llm::HttpTransport* SlackAdapter::get_transport() {
    if (transport_) return transport_;
    return hermes::llm::get_default_transport();
}

// ----- URL encoding / query string --------------------------------------

std::string SlackAdapter::urlencode(const std::string& s) {
    std::ostringstream os;
    os.fill('0');
    os << std::hex;
    for (unsigned char c : s) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            os << c;
        } else {
            os << '%' << std::setw(2) << static_cast<int>(c);
        }
    }
    return os.str();
}

std::string SlackAdapter::build_query_string(
    const std::unordered_map<std::string, std::string>& params) {
    std::ostringstream os;
    bool first = true;
    for (const auto& [k, v] : params) {
        if (!first) os << '&';
        first = false;
        os << urlencode(k) << '=' << urlencode(v);
    }
    return os.str();
}

// ----- Core HTTP wrapper with 429 retry ---------------------------------

hermes::llm::HttpTransport::Response SlackAdapter::post_api(
    const std::string& method, const nlohmann::json& payload) {
    auto* transport = get_transport();
    hermes::llm::HttpTransport::Response resp;
    resp.status_code = 0;
    last_retry_count_ = 0;
    if (!transport) return resp;

    std::string url = std::string(kSlackApi) + method;
    std::unordered_map<std::string, std::string> headers = {
        {"Authorization", "Bearer " + cfg_.bot_token},
        {"Content-Type", "application/json; charset=utf-8"},
    };

    for (int attempt = 0; attempt <= cfg_.max_retries_on_429; ++attempt) {
        try {
            resp = transport->post_json(url, headers, payload.dump());
        } catch (...) {
            resp.status_code = 0;
            return resp;
        }
        if (resp.status_code != 429) return resp;
        if (attempt == cfg_.max_retries_on_429) break;
        ++last_retry_count_;
        // retry_after_override_ms: <0 means "no sleep, bypass server hint",
        // 0 means "honor Retry-After header", >0 uses the override directly.
        std::int64_t delay_ms = 0;
        if (cfg_.retry_after_override_ms > 0) {
            delay_ms = cfg_.retry_after_override_ms;
        } else if (cfg_.retry_after_override_ms == 0) {
            delay_ms = parse_retry_after(resp.headers);
        }
        if (delay_ms > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
        }
    }
    return resp;
}

hermes::llm::HttpTransport::Response SlackAdapter::get_api(
    const std::string& method,
    const std::unordered_map<std::string, std::string>& query) {
    auto* transport = get_transport();
    hermes::llm::HttpTransport::Response resp;
    resp.status_code = 0;
    last_retry_count_ = 0;
    if (!transport) return resp;

    std::string url = std::string(kSlackApi) + method;
    std::string qs = build_query_string(query);
    if (!qs.empty()) url += "?" + qs;
    std::unordered_map<std::string, std::string> headers = {
        {"Authorization", "Bearer " + cfg_.bot_token},
    };
    for (int attempt = 0; attempt <= cfg_.max_retries_on_429; ++attempt) {
        try {
            resp = transport->get(url, headers);
        } catch (...) {
            resp.status_code = 0;
            return resp;
        }
        if (resp.status_code != 429) return resp;
        if (attempt == cfg_.max_retries_on_429) break;
        ++last_retry_count_;
        std::int64_t delay_ms = 0;
        if (cfg_.retry_after_override_ms > 0) {
            delay_ms = cfg_.retry_after_override_ms;
        } else if (cfg_.retry_after_override_ms == 0) {
            delay_ms = parse_retry_after(resp.headers);
        }
        if (delay_ms > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
        }
    }
    return resp;
}

// ----- Lifecycle --------------------------------------------------------

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
        if (body.contains("user_id") && cfg_.bot_user_id.empty()) {
            cfg_.bot_user_id = body["user_id"].get<std::string>();
        }
        if (body.contains("team_id") && cfg_.team_id.empty()) {
            cfg_.team_id = body["team_id"].get<std::string>();
        }
        if (body.contains("enterprise_id") && cfg_.enterprise_id.empty() &&
            body["enterprise_id"].is_string()) {
            cfg_.enterprise_id = body["enterprise_id"].get<std::string>();
        }
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

// ----- Event parsing ----------------------------------------------------

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
    if (event.contains("bot_id") && event.value("bot_id", "").size() > 0) {
        return false;
    }
    if (event.contains("subtype")) return false;

    auto channel = event.value("channel", "");
    if (!channel.empty() && channel[0] == 'D') return true;
    if (parse_thread_ts(event).has_value()) return true;

    if (!bot_user_id.empty()) {
        auto text = event.value("text", "");
        std::string needle = "<@" + bot_user_id + ">";
        if (text.find(needle) != std::string::npos) return true;
    }
    return false;
}

SlackAdapter::GridIdentity SlackAdapter::parse_grid_identity(
    const nlohmann::json& envelope) {
    GridIdentity out;
    if (!envelope.is_object()) return out;
    // Events API payloads put team_id / enterprise_id at top level.
    if (envelope.contains("team_id") && envelope["team_id"].is_string()) {
        out.team_id = envelope["team_id"].get<std::string>();
    }
    if (envelope.contains("enterprise_id") &&
        envelope["enterprise_id"].is_string()) {
        out.enterprise_id = envelope["enterprise_id"].get<std::string>();
    }
    if (envelope.contains("is_enterprise_install") &&
        envelope["is_enterprise_install"].is_boolean()) {
        out.is_enterprise_install =
            envelope["is_enterprise_install"].get<bool>();
    }
    // Authed_teams / authorizations — fall back when top-level is absent.
    if (out.team_id.empty() && envelope.contains("authorizations") &&
        envelope["authorizations"].is_array() &&
        !envelope["authorizations"].empty()) {
        const auto& a = envelope["authorizations"][0];
        if (a.contains("team_id") && a["team_id"].is_string()) {
            out.team_id = a["team_id"].get<std::string>();
        }
        if (a.contains("enterprise_id") && a["enterprise_id"].is_string()) {
            out.enterprise_id = a["enterprise_id"].get<std::string>();
        }
        if (a.contains("is_enterprise_install") &&
            a["is_enterprise_install"].is_boolean()) {
            out.is_enterprise_install =
                a["is_enterprise_install"].get<bool>();
        }
    }
    return out;
}

bool SlackAdapter::is_ignorable_subtype(const nlohmann::json& event) {
    if (!event.is_object() || !event.contains("subtype")) return false;
    static const std::unordered_set<std::string> kIgnore = {
        "bot_message",    "message_changed", "message_deleted",
        "channel_join",   "channel_leave",   "channel_topic",
        "channel_purpose", "channel_name",   "pinned_item",
        "unpinned_item",  "thread_broadcast"};
    auto s = event.value("subtype", "");
    return kIgnore.count(s) > 0;
}

// ----- Signature verification -------------------------------------------

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
    return "v0=" + to_hex(digest, digest_len);
}

bool SlackAdapter::verify_events_api_signature(
    const std::string& signing_secret,
    const std::string& timestamp,
    const std::string& body,
    const std::string& signature,
    int max_skew_seconds) {
    if (signing_secret.empty() || timestamp.empty() || signature.empty()) {
        return false;
    }
    // Reject stale timestamps to block replay.
    try {
        std::int64_t ts = std::stoll(timestamp);
        std::int64_t now = static_cast<std::int64_t>(std::time(nullptr));
        if (std::llabs(now - ts) > max_skew_seconds) return false;
    } catch (...) {
        return false;
    }
    auto expected = compute_slack_signature(signing_secret, timestamp, body);
    return constant_time_equals(expected, signature);
}

// ----- Block Kit builders -----------------------------------------------

nlohmann::json SlackAdapter::section_block(const std::string& markdown_text,
                                           const std::string& block_id) {
    nlohmann::json b = {
        {"type", "section"},
        {"text", {{"type", "mrkdwn"}, {"text", markdown_text}}},
    };
    if (!block_id.empty()) b["block_id"] = block_id;
    return b;
}

nlohmann::json SlackAdapter::divider_block() {
    return nlohmann::json{{"type", "divider"}};
}

nlohmann::json SlackAdapter::header_block(const std::string& plain_text) {
    return nlohmann::json{
        {"type", "header"},
        {"text", {{"type", "plain_text"}, {"text", plain_text}}},
    };
}

nlohmann::json SlackAdapter::context_block(
    const std::vector<std::string>& elements) {
    nlohmann::json arr = nlohmann::json::array();
    for (const auto& e : elements) {
        arr.push_back({{"type", "mrkdwn"}, {"text", e}});
    }
    return nlohmann::json{{"type", "context"}, {"elements", arr}};
}

nlohmann::json SlackAdapter::image_block(const std::string& image_url,
                                         const std::string& alt_text,
                                         const std::string& title) {
    nlohmann::json b = {
        {"type", "image"},
        {"image_url", image_url},
        {"alt_text", alt_text},
    };
    if (!title.empty()) {
        b["title"] = {{"type", "plain_text"}, {"text", title}};
    }
    return b;
}

nlohmann::json SlackAdapter::input_block(const std::string& label,
                                         const nlohmann::json& element,
                                         const std::string& block_id,
                                         bool optional) {
    nlohmann::json b = {
        {"type", "input"},
        {"label", {{"type", "plain_text"}, {"text", label}}},
        {"element", element},
    };
    if (!block_id.empty()) b["block_id"] = block_id;
    if (optional) b["optional"] = true;
    return b;
}

nlohmann::json SlackAdapter::actions_block(
    const std::vector<nlohmann::json>& elements, const std::string& block_id) {
    nlohmann::json b = {{"type", "actions"}, {"elements", elements}};
    if (!block_id.empty()) b["block_id"] = block_id;
    return b;
}

nlohmann::json SlackAdapter::button_element(const std::string& text,
                                            const std::string& action_id,
                                            const std::string& value,
                                            const std::string& style) {
    nlohmann::json e = {
        {"type", "button"},
        {"text", {{"type", "plain_text"}, {"text", text}}},
        {"action_id", action_id},
    };
    if (!value.empty()) e["value"] = value;
    if (!style.empty()) e["style"] = style;  // "primary" / "danger"
    return e;
}

nlohmann::json SlackAdapter::static_select_element(
    const std::string& action_id,
    const std::string& placeholder,
    const std::vector<std::pair<std::string, std::string>>& options) {
    nlohmann::json opts = nlohmann::json::array();
    for (const auto& [label, value] : options) {
        opts.push_back({
            {"text", {{"type", "plain_text"}, {"text", label}}},
            {"value", value},
        });
    }
    return nlohmann::json{
        {"type", "static_select"},
        {"action_id", action_id},
        {"placeholder", {{"type", "plain_text"}, {"text", placeholder}}},
        {"options", opts},
    };
}

nlohmann::json SlackAdapter::datepicker_element(
    const std::string& action_id,
    const std::string& placeholder,
    const std::string& initial_date) {
    nlohmann::json e = {
        {"type", "datepicker"},
        {"action_id", action_id},
        {"placeholder", {{"type", "plain_text"}, {"text", placeholder}}},
    };
    if (!initial_date.empty()) e["initial_date"] = initial_date;
    return e;
}

nlohmann::json SlackAdapter::users_select_element(
    const std::string& action_id, const std::string& placeholder) {
    return nlohmann::json{
        {"type", "users_select"},
        {"action_id", action_id},
        {"placeholder", {{"type", "plain_text"}, {"text", placeholder}}},
    };
}

nlohmann::json SlackAdapter::channels_select_element(
    const std::string& action_id, const std::string& placeholder) {
    return nlohmann::json{
        {"type", "channels_select"},
        {"action_id", action_id},
        {"placeholder", {{"type", "plain_text"}, {"text", placeholder}}},
    };
}

nlohmann::json SlackAdapter::plain_text_input_element(
    const std::string& action_id, bool multiline) {
    return nlohmann::json{
        {"type", "plain_text_input"},
        {"action_id", action_id},
        {"multiline", multiline},
    };
}

nlohmann::json SlackAdapter::modal_view(
    const std::string& title, const std::vector<nlohmann::json>& blocks,
    const std::string& submit, const std::string& close,
    const std::string& callback_id, const std::string& private_metadata) {
    nlohmann::json v = {
        {"type", "modal"},
        {"title", {{"type", "plain_text"}, {"text", title}}},
        {"blocks", blocks},
    };
    if (!submit.empty()) {
        v["submit"] = {{"type", "plain_text"}, {"text", submit}};
    }
    if (!close.empty()) {
        v["close"] = {{"type", "plain_text"}, {"text", close}};
    }
    if (!callback_id.empty()) v["callback_id"] = callback_id;
    if (!private_metadata.empty()) v["private_metadata"] = private_metadata;
    return v;
}

nlohmann::json SlackAdapter::home_view(
    const std::vector<nlohmann::json>& blocks) {
    return nlohmann::json{{"type", "home"}, {"blocks", blocks}};
}

// ----- Message ops -------------------------------------------------------

bool SlackAdapter::send(const std::string& chat_id,
                        const std::string& content) {
    nlohmann::json payload = {{"channel", chat_id}, {"text", content}};
    auto resp = post_api("chat.postMessage", payload);
    if (resp.status_code != 200) return false;
    try {
        return nlohmann::json::parse(resp.body).value("ok", false);
    } catch (...) { return false; }
}

bool SlackAdapter::send_thread_reply(const std::string& chat_id,
                                     const std::string& thread_ts,
                                     const std::string& content) {
    nlohmann::json payload = {
        {"channel", chat_id}, {"text", content}, {"thread_ts", thread_ts},
    };
    auto resp = post_api("chat.postMessage", payload);
    if (resp.status_code != 200) return false;
    try {
        return nlohmann::json::parse(resp.body).value("ok", false);
    } catch (...) { return false; }
}

bool SlackAdapter::send_blocks(const std::string& chat_id,
                               const std::vector<nlohmann::json>& blocks,
                               const std::string& fallback_text,
                               const std::string& thread_ts) {
    nlohmann::json payload = {{"channel", chat_id}, {"blocks", blocks}};
    if (!fallback_text.empty()) payload["text"] = fallback_text;
    if (!thread_ts.empty()) payload["thread_ts"] = thread_ts;
    auto resp = post_api("chat.postMessage", payload);
    if (resp.status_code != 200) return false;
    try {
        return nlohmann::json::parse(resp.body).value("ok", false);
    } catch (...) { return false; }
}

bool SlackAdapter::send_ephemeral(const std::string& chat_id,
                                  const std::string& user,
                                  const std::string& content) {
    nlohmann::json payload = {
        {"channel", chat_id}, {"user", user}, {"text", content}};
    auto resp = post_api("chat.postEphemeral", payload);
    if (resp.status_code != 200) return false;
    try {
        return nlohmann::json::parse(resp.body).value("ok", false);
    } catch (...) { return false; }
}

bool SlackAdapter::update_message(const std::string& chat_id,
                                  const std::string& ts,
                                  const std::string& content) {
    nlohmann::json payload = {
        {"channel", chat_id}, {"ts", ts}, {"text", content}};
    auto resp = post_api("chat.update", payload);
    if (resp.status_code != 200) return false;
    try {
        return nlohmann::json::parse(resp.body).value("ok", false);
    } catch (...) { return false; }
}

bool SlackAdapter::delete_message(const std::string& chat_id,
                                  const std::string& ts) {
    nlohmann::json payload = {{"channel", chat_id}, {"ts", ts}};
    auto resp = post_api("chat.delete", payload);
    if (resp.status_code != 200) return false;
    try {
        return nlohmann::json::parse(resp.body).value("ok", false);
    } catch (...) { return false; }
}

void SlackAdapter::send_typing(const std::string& /*chat_id*/) {
    // Slack typing indicators require an open WebSocket (Socket Mode / RTM).
}

// ----- Files -------------------------------------------------------------

bool SlackAdapter::upload_file(const std::string& chat_id,
                               const std::string& filename,
                               const std::string& content,
                               const std::string& initial_comment) {
    nlohmann::json payload = {
        {"channels", chat_id}, {"filename", filename}, {"content", content},
    };
    if (!initial_comment.empty()) payload["initial_comment"] = initial_comment;
    auto resp = post_api("files.upload", payload);
    if (resp.status_code != 200) return false;
    try {
        return nlohmann::json::parse(resp.body).value("ok", false);
    } catch (...) { return false; }
}

SlackAdapter::UploadExternalResult SlackAdapter::get_upload_url_external(
    const std::string& filename, std::int64_t length) {
    UploadExternalResult r;
    auto resp =
        get_api("files.getUploadURLExternal",
                {{"filename", filename}, {"length", std::to_string(length)}});
    if (resp.status_code != 200) return r;
    try {
        auto body = nlohmann::json::parse(resp.body);
        if (!body.value("ok", false)) return r;
        r.ok = true;
        r.file_id = body.value("file_id", "");
        r.upload_url = body.value("upload_url", "");
    } catch (...) { }
    return r;
}

bool SlackAdapter::complete_upload_external(const std::string& file_id,
                                            const std::string& title,
                                            const std::string& channel_id,
                                            const std::string& initial_comment,
                                            const std::string& thread_ts) {
    nlohmann::json files = nlohmann::json::array();
    nlohmann::json f = {{"id", file_id}};
    if (!title.empty()) f["title"] = title;
    files.push_back(f);
    nlohmann::json payload = {{"files", files}};
    if (!channel_id.empty()) payload["channel_id"] = channel_id;
    if (!initial_comment.empty()) payload["initial_comment"] = initial_comment;
    if (!thread_ts.empty()) payload["thread_ts"] = thread_ts;
    auto resp = post_api("files.completeUploadExternal", payload);
    if (resp.status_code != 200) return false;
    try {
        return nlohmann::json::parse(resp.body).value("ok", false);
    } catch (...) { return false; }
}

// ----- Reactions / pins / stars -----------------------------------------

bool SlackAdapter::add_reaction(const std::string& channel,
                                const std::string& ts,
                                const std::string& emoji) {
    auto resp = post_api("reactions.add",
                         {{"channel", channel}, {"timestamp", ts},
                          {"name", emoji}});
    if (resp.status_code != 200) return false;
    try {
        return nlohmann::json::parse(resp.body).value("ok", false);
    } catch (...) { return false; }
}

bool SlackAdapter::remove_reaction(const std::string& channel,
                                   const std::string& ts,
                                   const std::string& emoji) {
    auto resp = post_api("reactions.remove",
                         {{"channel", channel}, {"timestamp", ts},
                          {"name", emoji}});
    if (resp.status_code != 200) return false;
    try {
        return nlohmann::json::parse(resp.body).value("ok", false);
    } catch (...) { return false; }
}

nlohmann::json SlackAdapter::get_reactions(const std::string& channel,
                                           const std::string& ts) {
    auto resp = get_api("reactions.get", {{"channel", channel},
                                           {"timestamp", ts}});
    if (resp.status_code != 200) return nlohmann::json::object();
    try { return nlohmann::json::parse(resp.body); }
    catch (...) { return nlohmann::json::object(); }
}

bool SlackAdapter::pin_message(const std::string& channel,
                               const std::string& ts) {
    auto resp = post_api("pins.add", {{"channel", channel}, {"timestamp", ts}});
    if (resp.status_code != 200) return false;
    try {
        return nlohmann::json::parse(resp.body).value("ok", false);
    } catch (...) { return false; }
}

bool SlackAdapter::unpin_message(const std::string& channel,
                                 const std::string& ts) {
    auto resp = post_api("pins.remove",
                         {{"channel", channel}, {"timestamp", ts}});
    if (resp.status_code != 200) return false;
    try {
        return nlohmann::json::parse(resp.body).value("ok", false);
    } catch (...) { return false; }
}

bool SlackAdapter::add_star(const std::string& channel,
                            const std::string& ts) {
    auto resp = post_api("stars.add",
                         {{"channel", channel}, {"timestamp", ts}});
    if (resp.status_code != 200) return false;
    try {
        return nlohmann::json::parse(resp.body).value("ok", false);
    } catch (...) { return false; }
}

// ----- Conversations ----------------------------------------------------

std::optional<std::string> SlackAdapter::open_dm(const std::string& user_id) {
    auto resp = post_api("conversations.open", {{"users", user_id}});
    if (resp.status_code != 200) return std::nullopt;
    try {
        auto body = nlohmann::json::parse(resp.body);
        if (!body.value("ok", false)) return std::nullopt;
        if (body.contains("channel") && body["channel"].contains("id")) {
            return body["channel"]["id"].get<std::string>();
        }
    } catch (...) { }
    return std::nullopt;
}

std::optional<std::string> SlackAdapter::open_mpim(
    const std::vector<std::string>& user_ids) {
    std::string joined;
    for (std::size_t i = 0; i < user_ids.size(); ++i) {
        if (i) joined += ",";
        joined += user_ids[i];
    }
    auto resp = post_api("conversations.open", {{"users", joined}});
    if (resp.status_code != 200) return std::nullopt;
    try {
        auto body = nlohmann::json::parse(resp.body);
        if (!body.value("ok", false)) return std::nullopt;
        if (body.contains("channel") && body["channel"].contains("id")) {
            return body["channel"]["id"].get<std::string>();
        }
    } catch (...) { }
    return std::nullopt;
}

nlohmann::json SlackAdapter::get_channel_info(const std::string& channel_id) {
    auto resp = get_api("conversations.info", {{"channel", channel_id}});
    if (resp.status_code != 200) return nlohmann::json::object();
    try { return nlohmann::json::parse(resp.body); }
    catch (...) { return nlohmann::json::object(); }
}

nlohmann::json SlackAdapter::get_channel_members(
    const std::string& channel_id, int limit) {
    auto resp = get_api("conversations.members",
                        {{"channel", channel_id},
                         {"limit", std::to_string(limit)}});
    if (resp.status_code != 200) return nlohmann::json::object();
    try { return nlohmann::json::parse(resp.body); }
    catch (...) { return nlohmann::json::object(); }
}

nlohmann::json SlackAdapter::get_channel_history(
    const std::string& channel_id, int limit, const std::string& cursor) {
    std::unordered_map<std::string, std::string> q = {
        {"channel", channel_id}, {"limit", std::to_string(limit)}};
    if (!cursor.empty()) q["cursor"] = cursor;
    auto resp = get_api("conversations.history", q);
    if (resp.status_code != 200) return nlohmann::json::object();
    try { return nlohmann::json::parse(resp.body); }
    catch (...) { return nlohmann::json::object(); }
}

nlohmann::json SlackAdapter::list_channels(const std::string& types,
                                           int limit) {
    auto resp = get_api("conversations.list",
                        {{"types", types}, {"limit", std::to_string(limit)}});
    if (resp.status_code != 200) return nlohmann::json::object();
    try { return nlohmann::json::parse(resp.body); }
    catch (...) { return nlohmann::json::object(); }
}

// ----- Users -----------------------------------------------------------

nlohmann::json SlackAdapter::get_user_info(const std::string& user_id) {
    auto resp = get_api("users.info", {{"user", user_id}});
    if (resp.status_code != 200) return nlohmann::json::object();
    try { return nlohmann::json::parse(resp.body); }
    catch (...) { return nlohmann::json::object(); }
}

std::optional<std::string> SlackAdapter::lookup_user_by_email(
    const std::string& email) {
    auto resp = get_api("users.lookupByEmail", {{"email", email}});
    if (resp.status_code != 200) return std::nullopt;
    try {
        auto body = nlohmann::json::parse(resp.body);
        if (!body.value("ok", false)) return std::nullopt;
        if (body.contains("user") && body["user"].contains("id")) {
            return body["user"]["id"].get<std::string>();
        }
    } catch (...) { }
    return std::nullopt;
}

std::optional<std::string> SlackAdapter::get_user_presence(
    const std::string& user_id) {
    auto resp = get_api("users.getPresence", {{"user", user_id}});
    if (resp.status_code != 200) return std::nullopt;
    try {
        auto body = nlohmann::json::parse(resp.body);
        if (!body.value("ok", false)) return std::nullopt;
        if (body.contains("presence") && body["presence"].is_string()) {
            return body["presence"].get<std::string>();
        }
    } catch (...) { }
    return std::nullopt;
}

// ----- Views ----------------------------------------------------------

bool SlackAdapter::views_open(const std::string& trigger_id,
                              const nlohmann::json& view) {
    auto resp = post_api("views.open",
                         {{"trigger_id", trigger_id}, {"view", view}});
    if (resp.status_code != 200) return false;
    try {
        return nlohmann::json::parse(resp.body).value("ok", false);
    } catch (...) { return false; }
}

bool SlackAdapter::views_push(const std::string& trigger_id,
                              const nlohmann::json& view) {
    auto resp = post_api("views.push",
                         {{"trigger_id", trigger_id}, {"view", view}});
    if (resp.status_code != 200) return false;
    try {
        return nlohmann::json::parse(resp.body).value("ok", false);
    } catch (...) { return false; }
}

bool SlackAdapter::views_update(const std::string& view_id,
                                const nlohmann::json& view) {
    auto resp = post_api("views.update",
                         {{"view_id", view_id}, {"view", view}});
    if (resp.status_code != 200) return false;
    try {
        return nlohmann::json::parse(resp.body).value("ok", false);
    } catch (...) { return false; }
}

bool SlackAdapter::views_publish(const std::string& user_id,
                                 const nlohmann::json& view,
                                 const std::string& hash) {
    nlohmann::json payload = {{"user_id", user_id}, {"view", view}};
    if (!hash.empty()) payload["hash"] = hash;
    auto resp = post_api("views.publish", payload);
    if (resp.status_code != 200) return false;
    try {
        return nlohmann::json::parse(resp.body).value("ok", false);
    } catch (...) { return false; }
}

// ----- mrkdwn formatting ------------------------------------------------

std::string SlackAdapter::format_message(const std::string& content) {
    if (content.empty()) return content;

    // Placeholder store — mirrors the Python _ph() pattern but using a
    // simple vector since NUL markers survive std::string operations.
    std::vector<std::string> placeholders;
    auto ph = [&placeholders](const std::string& s) {
        std::string key = "\x01SL" + std::to_string(placeholders.size()) +
                          "\x01";
        placeholders.push_back(s);
        return key;
    };

    std::string text = content;

    // 1) Protect fenced code blocks.
    {
        std::regex re(R"((```(?:[^\n]*\n)?[\s\S]*?```))");
        std::string out;
        auto begin = text.cbegin();
        std::smatch m;
        while (std::regex_search(begin, text.cend(), m, re)) {
            out.append(m.prefix().first, m.prefix().second);
            out += ph(m[0].str());
            begin = m.suffix().first;
        }
        out.append(begin, text.cend());
        text = out;
    }

    // 2) Inline code `x`.
    {
        std::regex re(R"((`[^`\n]+`))");
        std::string out;
        auto begin = text.cbegin();
        std::smatch m;
        while (std::regex_search(begin, text.cend(), m, re)) {
            out.append(m.prefix().first, m.prefix().second);
            out += ph(m[0].str());
            begin = m.suffix().first;
        }
        out.append(begin, text.cend());
        text = out;
    }

    // 3) Markdown links [label](url) → <url|label>.
    {
        std::regex re(R"(\[([^\]]+)\]\(([^)\s]+)\))");
        std::string out;
        auto begin = text.cbegin();
        std::smatch m;
        while (std::regex_search(begin, text.cend(), m, re)) {
            out.append(m.prefix().first, m.prefix().second);
            out += ph("<" + m[2].str() + "|" + m[1].str() + ">");
            begin = m.suffix().first;
        }
        out.append(begin, text.cend());
        text = out;
    }

    // 4) Protect existing Slack entities.
    {
        std::regex re(R"((<(?:[@#!]|(?:https?|mailto|tel):)[^>\n]+>))");
        std::string out;
        auto begin = text.cbegin();
        std::smatch m;
        while (std::regex_search(begin, text.cend(), m, re)) {
            out.append(m.prefix().first, m.prefix().second);
            out += ph(m[0].str());
            begin = m.suffix().first;
        }
        out.append(begin, text.cend());
        text = out;
    }

    // 5) Escape Slack control characters on remaining plain text.
    auto replace_all = [](std::string s, const std::string& from,
                          const std::string& to) {
        std::string out;
        out.reserve(s.size());
        std::size_t pos = 0, found;
        while ((found = s.find(from, pos)) != std::string::npos) {
            out.append(s, pos, found - pos);
            out += to;
            pos = found + from.size();
        }
        out.append(s, pos, std::string::npos);
        return out;
    };
    text = replace_all(text, "&amp;", "&");
    text = replace_all(text, "&lt;", "<");
    text = replace_all(text, "&gt;", ">");
    text = replace_all(text, "&", "&amp;");
    text = replace_all(text, "<", "&lt;");
    text = replace_all(text, ">", "&gt;");

    // 6) Headers (## Title) → *Title*.
    {
        std::regex re(R"(^#{1,6}\s+(.+)$)", std::regex::multiline);
        std::string out;
        auto begin = text.cbegin();
        std::smatch m;
        while (std::regex_search(begin, text.cend(), m, re)) {
            out.append(m.prefix().first, m.prefix().second);
            std::string inner = m[1].str();
            // Strip redundant **bold** inside a header.
            inner = std::regex_replace(inner, std::regex(R"(\*\*(.+?)\*\*)"),
                                       "$1");
            out += ph("*" + inner + "*");
            begin = m.suffix().first;
        }
        out.append(begin, text.cend());
        text = out;
    }

    // 7) ***bold-italic*** → *_x_*.
    {
        std::regex re(R"(\*\*\*(.+?)\*\*\*)");
        std::string out;
        auto begin = text.cbegin();
        std::smatch m;
        while (std::regex_search(begin, text.cend(), m, re)) {
            out.append(m.prefix().first, m.prefix().second);
            out += ph("*_" + m[1].str() + "_*");
            begin = m.suffix().first;
        }
        out.append(begin, text.cend());
        text = out;
    }

    // 8) **bold** → *bold*.
    {
        std::regex re(R"(\*\*(.+?)\*\*)");
        std::string out;
        auto begin = text.cbegin();
        std::smatch m;
        while (std::regex_search(begin, text.cend(), m, re)) {
            out.append(m.prefix().first, m.prefix().second);
            out += ph("*" + m[1].str() + "*");
            begin = m.suffix().first;
        }
        out.append(begin, text.cend());
        text = out;
    }

    // 9) ~~strike~~ → ~strike~.
    {
        std::regex re(R"(~~(.+?)~~)");
        std::string out;
        auto begin = text.cbegin();
        std::smatch m;
        while (std::regex_search(begin, text.cend(), m, re)) {
            out.append(m.prefix().first, m.prefix().second);
            out += ph("~" + m[1].str() + "~");
            begin = m.suffix().first;
        }
        out.append(begin, text.cend());
        text = out;
    }

    // 10) Restore placeholders — iterate in reverse so nested substitutions
    // come back in their original order.
    for (std::size_t i = placeholders.size(); i-- > 0;) {
        std::string key = "\x01SL" + std::to_string(i) + "\x01";
        text = replace_all(text, key, placeholders[i]);
    }
    return text;
}

// ----- Slash command form parser ---------------------------------------

std::unordered_map<std::string, std::string> SlackAdapter::parse_slash_form(
    const std::string& body) {
    std::unordered_map<std::string, std::string> out;
    auto percent_decode = [](const std::string& in) {
        std::string o;
        o.reserve(in.size());
        for (std::size_t i = 0; i < in.size(); ++i) {
            char c = in[i];
            if (c == '+') { o.push_back(' '); continue; }
            if (c == '%' && i + 2 < in.size()) {
                int hi = std::isxdigit(static_cast<unsigned char>(in[i + 1]))
                             ? in[i + 1] : 0;
                int lo = std::isxdigit(static_cast<unsigned char>(in[i + 2]))
                             ? in[i + 2] : 0;
                auto fromhex = [](char x) {
                    if (x >= '0' && x <= '9') return x - '0';
                    if (x >= 'a' && x <= 'f') return 10 + x - 'a';
                    if (x >= 'A' && x <= 'F') return 10 + x - 'A';
                    return 0;
                };
                o.push_back(static_cast<char>((fromhex(hi) << 4) | fromhex(lo)));
                i += 2;
                continue;
            }
            o.push_back(c);
        }
        return o;
    };

    std::size_t pos = 0;
    while (pos < body.size()) {
        std::size_t amp = body.find('&', pos);
        std::string pair = body.substr(
            pos, amp == std::string::npos ? std::string::npos : amp - pos);
        std::size_t eq = pair.find('=');
        if (eq != std::string::npos) {
            out[percent_decode(pair.substr(0, eq))] =
                percent_decode(pair.substr(eq + 1));
        } else if (!pair.empty()) {
            out[percent_decode(pair)] = "";
        }
        if (amp == std::string::npos) break;
        pos = amp + 1;
    }
    return out;
}

// ----- Socket Mode ------------------------------------------------------

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
            const nlohmann::json* ev = &data;
            nlohmann::json payload_event;
            if (data.contains("event") && data["event"].is_object()) {
                payload_event = data["event"];
                ev = &payload_event;
            }
            // Interactivity envelopes (block_actions / view_submission /
            // shortcut / view_closed) surface directly under the envelope.
            std::string env_type = data.value("type", type);
            if (env_type == "interactive" || env_type == "block_actions" ||
                env_type == "view_submission" || env_type == "shortcut" ||
                env_type == "message_action") {
                if (interaction_cb_) interaction_cb_(data);
                return;
            }

            std::string inner_type = ev->value("type", type);
            if (inner_type != "message") return;
            if (!message_cb_) return;
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

    auto* raw_sm = socket_mode_.get();
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
