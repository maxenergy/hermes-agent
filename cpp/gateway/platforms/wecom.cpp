// Phase 12 — WeCom (Enterprise WeChat) adapter implementation (depth port).
#include "wecom.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstring>
#include <iomanip>
#include <random>
#include <regex>
#include <sstream>

#include <nlohmann/json.hpp>

namespace hermes::gateway::platforms {

namespace {

std::string to_lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string trim(std::string s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.erase(s.begin());
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.pop_back();
    return s;
}

}  // namespace

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

std::vector<std::string> wecom_coerce_list(const nlohmann::json& value) {
    std::vector<std::string> out;
    if (value.is_null()) return out;
    if (value.is_string()) {
        std::string s = value.get<std::string>();
        std::stringstream ss(s);
        std::string item;
        while (std::getline(ss, item, ',')) {
            item = trim(item);
            if (!item.empty()) out.push_back(item);
        }
        return out;
    }
    if (value.is_array()) {
        for (const auto& v : value) {
            std::string item;
            if (v.is_string()) item = v.get<std::string>();
            else item = v.dump();
            item = trim(item);
            if (!item.empty()) out.push_back(item);
        }
        return out;
    }
    std::string item = trim(value.dump());
    if (!item.empty()) out.push_back(item);
    return out;
}

std::string wecom_normalize_entry(const std::string& raw) {
    std::string value = trim(raw);
    // strip wecom: prefix (case-insensitive)
    std::string lower = to_lower(value);
    auto maybe_strip = [&](const std::string& prefix) {
        if (lower.size() >= prefix.size() &&
            lower.compare(0, prefix.size(), prefix) == 0) {
            value = value.substr(prefix.size());
            lower = to_lower(value);
        }
    };
    maybe_strip("wecom:");
    maybe_strip("user:");
    maybe_strip("group:");
    return trim(value);
}

bool wecom_entry_matches(const std::vector<std::string>& entries,
                         const std::string& target) {
    std::string needle = to_lower(trim(target));
    for (const auto& e : entries) {
        std::string n = to_lower(wecom_normalize_entry(e));
        if (n == "*" || n == needle) return true;
    }
    return false;
}

std::string wecom_detect_image_ext(const std::string& bytes) {
    if (bytes.size() >= 8 &&
        static_cast<unsigned char>(bytes[0]) == 0x89 && bytes[1] == 'P' &&
        bytes[2] == 'N' && bytes[3] == 'G') return ".png";
    if (bytes.size() >= 3 &&
        static_cast<unsigned char>(bytes[0]) == 0xFF &&
        static_cast<unsigned char>(bytes[1]) == 0xD8 &&
        static_cast<unsigned char>(bytes[2]) == 0xFF) return ".jpg";
    if (bytes.size() >= 6 && bytes.compare(0, 6, "GIF89a") == 0) return ".gif";
    if (bytes.size() >= 6 && bytes.compare(0, 6, "GIF87a") == 0) return ".gif";
    if (bytes.size() >= 12 && bytes.compare(0, 4, "RIFF") == 0 &&
        bytes.compare(8, 4, "WEBP") == 0) return ".webp";
    return ".bin";
}

std::string wecom_mime_for_ext(const std::string& ext,
                               const std::string& fallback) {
    static const std::unordered_map<std::string, std::string> kMap = {
        {".png", "image/png"}, {".jpg", "image/jpeg"},
        {".jpeg", "image/jpeg"}, {".gif", "image/gif"},
        {".webp", "image/webp"}, {".mp4", "video/mp4"},
        {".mov", "video/quicktime"}, {".mp3", "audio/mpeg"},
        {".aac", "audio/aac"}, {".amr", "audio/amr"},
        {".wav", "audio/wav"}, {".pdf", "application/pdf"},
        {".txt", "text/plain"}, {".zip", "application/zip"},
        {".doc", "application/msword"},
    };
    std::string lower = to_lower(ext);
    auto it = kMap.find(lower);
    if (it != kMap.end()) return it->second;
    return fallback;
}

std::string wecom_guess_extension(const std::string& url,
                                  const std::string& content_type,
                                  const std::string& fallback) {
    // URL path ext.
    auto q = url.find('?');
    std::string path = (q == std::string::npos) ? url : url.substr(0, q);
    auto slash = path.rfind('/');
    std::string last = (slash == std::string::npos) ? path : path.substr(slash + 1);
    auto dot = last.rfind('.');
    if (dot != std::string::npos && dot > 0) {
        std::string ext = to_lower(last.substr(dot));
        if (ext.size() <= 6) return ext;
    }
    // Fall back to content-type map.
    std::string ct = to_lower(content_type);
    if (ct.find("image/png") != std::string::npos) return ".png";
    if (ct.find("image/jpeg") != std::string::npos) return ".jpg";
    if (ct.find("image/gif") != std::string::npos) return ".gif";
    if (ct.find("image/webp") != std::string::npos) return ".webp";
    if (ct.find("video/mp4") != std::string::npos) return ".mp4";
    if (ct.find("audio/amr") != std::string::npos) return ".amr";
    if (ct.find("audio/mpeg") != std::string::npos) return ".mp3";
    if (ct.find("application/pdf") != std::string::npos) return ".pdf";
    return fallback;
}

std::string wecom_guess_filename(const std::string& url,
                                 const std::string& content_disposition,
                                 const std::string& content_type) {
    // RFC 6266 filename parameter.
    std::regex re(R"(filename\*?=(?:UTF-8'')?\"?([^\";]+)\"?)",
                  std::regex::icase);
    std::smatch m;
    if (std::regex_search(content_disposition, m, re)) {
        return trim(m[1].str());
    }
    auto q = url.find('?');
    std::string path = (q == std::string::npos) ? url : url.substr(0, q);
    auto slash = path.rfind('/');
    std::string last = (slash == std::string::npos) ? path : path.substr(slash + 1);
    if (!last.empty() && last.find('.') != std::string::npos) return last;
    return "download" + wecom_guess_extension(url, content_type);
}

std::string wecom_detect_media_type(const std::string& content_type) {
    std::string ct = to_lower(content_type);
    if (ct.substr(0, 6) == "image/") return "image";
    if (ct.substr(0, 6) == "video/") return "video";
    if (ct.substr(0, 6) == "audio/") return "voice";
    return "file";
}

bool wecom_apply_size_limits(std::size_t file_size,
                             const std::string& detected_type,
                             std::string* err_out) {
    std::size_t cap = kWeComFileMaxBytes;
    if (detected_type == "image") cap = kWeComImageMaxBytes;
    else if (detected_type == "video") cap = kWeComVideoMaxBytes;
    else if (detected_type == "voice") cap = kWeComVoiceMaxBytes;
    if (file_size > cap) {
        if (err_out) {
            *err_out = "wecom: " + detected_type + " too large: " +
                       std::to_string(file_size) + " > " + std::to_string(cap);
        }
        return false;
    }
    return true;
}

std::vector<std::string> wecom_split_text(const std::string& content,
                                          std::size_t max_length) {
    if (content.size() <= max_length) return {content};
    std::vector<std::string> out;
    // Split on paragraph boundaries first.
    std::vector<std::string> paragraphs;
    std::string cur;
    std::size_t nl = 0;
    for (std::size_t i = 0; i < content.size(); ++i) {
        cur.push_back(content[i]);
        if (content[i] == '\n') {
            if (++nl == 2) {
                paragraphs.push_back(cur);
                cur.clear();
                nl = 0;
            }
        } else {
            nl = 0;
        }
    }
    if (!cur.empty()) paragraphs.push_back(cur);

    std::string buf;
    for (auto& p : paragraphs) {
        if (p.size() > max_length) {
            if (!buf.empty()) { out.push_back(buf); buf.clear(); }
            for (std::size_t i = 0; i < p.size(); i += max_length) {
                out.push_back(p.substr(i, max_length));
            }
            continue;
        }
        if (buf.size() + p.size() > max_length && !buf.empty()) {
            out.push_back(buf);
            buf.clear();
        }
        buf += p;
    }
    if (!buf.empty()) out.push_back(buf);
    return out;
}

std::string wecom_derive_message_type(const nlohmann::json& body,
                                      const std::string& text,
                                      const std::vector<std::string>& media_types) {
    if (!media_types.empty()) {
        const auto& t = media_types.front();
        if (t == "image") return "PHOTO";
        if (t == "video") return "VIDEO";
        if (t == "voice") return "VOICE";
        if (t == "file") return "DOCUMENT";
    }
    std::string msgtype;
    if (body.contains("msgtype") && body["msgtype"].is_string()) {
        msgtype = body["msgtype"].get<std::string>();
    }
    if (msgtype == "markdown") return "TEXT";
    if (msgtype == "template_card") return "TEXT";
    (void)text;
    return "TEXT";
}

// ---------------------------------------------------------------------------
// Dedup cache
// ---------------------------------------------------------------------------

bool WeComDedupCache::check_and_add(
    const std::string& msg_id,
    std::chrono::steady_clock::time_point now) {
    std::lock_guard<std::mutex> lk(mu_);
    // GC expired.
    while (!order_.empty()) {
        const auto& head = order_.front();
        auto it = seen_.find(head);
        if (it == seen_.end()) { order_.pop_front(); continue; }
        if (std::chrono::duration<double>(now - it->second).count() > ttl_) {
            seen_.erase(it);
            order_.pop_front();
            continue;
        }
        break;
    }
    if (seen_.count(msg_id)) return false;
    seen_[msg_id] = now;
    order_.push_back(msg_id);
    while (order_.size() > cap_) {
        seen_.erase(order_.front());
        order_.pop_front();
    }
    return true;
}

std::size_t WeComDedupCache::size() const {
    std::lock_guard<std::mutex> lk(mu_);
    return seen_.size();
}

void WeComDedupCache::clear() {
    std::lock_guard<std::mutex> lk(mu_);
    seen_.clear();
    order_.clear();
}

// ---------------------------------------------------------------------------
// Policy enum ser/deser
// ---------------------------------------------------------------------------

WeComDmPolicy parse_wecom_dm_policy(std::string_view s) {
    if (s == "allowlist") return WeComDmPolicy::Allowlist;
    if (s == "disabled") return WeComDmPolicy::Disabled;
    if (s == "pairing") return WeComDmPolicy::Pairing;
    return WeComDmPolicy::Open;
}

WeComGroupPolicy parse_wecom_group_policy(std::string_view s) {
    if (s == "allowlist") return WeComGroupPolicy::Allowlist;
    if (s == "disabled") return WeComGroupPolicy::Disabled;
    return WeComGroupPolicy::Open;
}

std::string_view to_string(WeComDmPolicy p) {
    switch (p) {
        case WeComDmPolicy::Open: return "open";
        case WeComDmPolicy::Allowlist: return "allowlist";
        case WeComDmPolicy::Disabled: return "disabled";
        case WeComDmPolicy::Pairing: return "pairing";
    }
    return "open";
}

std::string_view to_string(WeComGroupPolicy p) {
    switch (p) {
        case WeComGroupPolicy::Open: return "open";
        case WeComGroupPolicy::Allowlist: return "allowlist";
        case WeComGroupPolicy::Disabled: return "disabled";
    }
    return "open";
}

// ---------------------------------------------------------------------------
// Adapter
// ---------------------------------------------------------------------------

WeComAdapter::WeComAdapter(Config cfg)
    : cfg_(std::move(cfg)), dedup_() {}

WeComAdapter::WeComAdapter(Config cfg, hermes::llm::HttpTransport* transport)
    : WeComAdapter(std::move(cfg)) {
    transport_ = transport;
}

hermes::llm::HttpTransport* WeComAdapter::get_transport() {
    if (transport_) return transport_;
    return hermes::llm::get_default_transport();
}

bool WeComAdapter::connect() {
    last_error_.clear();
    last_error_kind_ = AdapterErrorKind::None;
    if (cfg_.bot_id.empty() && cfg_.webhook_url.empty() &&
        cfg_.corp_id.empty()) {
        last_error_ = "no credentials configured";
        last_error_kind_ = AdapterErrorKind::Fatal;
        return false;
    }
    // Corporate REST mode: fetch an access_token eagerly.
    if (!cfg_.corp_id.empty() && !cfg_.corp_secret.empty()) {
        if (!refresh_access_token()) return false;
    }
    connected_ = true;
    return true;
}

void WeComAdapter::disconnect() {
    connected_ = false;
    access_token_.clear();
    {
        std::lock_guard<std::mutex> lk(pending_mu_);
        pending_.clear();
    }
}

bool WeComAdapter::refresh_access_token() {
    auto* transport = get_transport();
    if (!transport) return false;
    std::string url =
        "https://qyapi.weixin.qq.com/cgi-bin/gettoken?corpid=" + cfg_.corp_id +
        "&corpsecret=" + cfg_.corp_secret;
    try {
        auto resp = transport->get(url, {});
        if (resp.status_code != 200) {
            last_error_kind_ = AdapterErrorKind::Retryable;
            last_error_ = "gettoken HTTP " + std::to_string(resp.status_code);
            return false;
        }
        auto body = nlohmann::json::parse(resp.body);
        if (body.value("errcode", -1) != 0) {
            last_error_kind_ = AdapterErrorKind::Fatal;
            last_error_ = "gettoken errcode=" +
                          std::to_string(body.value("errcode", -1));
            return false;
        }
        access_token_ = body.value("access_token", "");
        int expires_in = body.value("expires_in", 7200);
        access_token_expiry_ = std::chrono::steady_clock::now() +
                                std::chrono::seconds(expires_in - 60);
        return !access_token_.empty();
    } catch (const std::exception& e) {
        last_error_kind_ = AdapterErrorKind::Retryable;
        last_error_ = e.what();
        return false;
    }
}

bool WeComAdapter::send_webhook_message(const std::string& content,
                                        const std::string& msgtype) {
    if (cfg_.webhook_url.empty()) return false;
    auto* transport = get_transport();
    if (!transport) return false;
    nlohmann::json payload;
    if (msgtype == "text") {
        payload = {{"msgtype", "text"}, {"text", {{"content", content}}}};
    } else if (msgtype == "markdown") {
        payload = {{"msgtype", "markdown"}, {"markdown", {{"content", content}}}};
    } else {
        payload = {{"msgtype", msgtype}, {msgtype, {{"content", content}}}};
    }
    try {
        auto resp = transport->post_json(
            cfg_.webhook_url,
            {{"Content-Type", "application/json"}},
            payload.dump());
        if (resp.status_code < 200 || resp.status_code >= 300) return false;
        auto body = nlohmann::json::parse(resp.body);
        return body.value("errcode", -1) == 0;
    } catch (...) {
        return false;
    }
}

bool WeComAdapter::send_markdown_via_api(const std::string& chat_id,
                                         const std::string& markdown) {
    if (access_token_.empty() && !refresh_access_token()) return false;
    auto* transport = get_transport();
    if (!transport) return false;
    nlohmann::json payload = {
        {"touser", chat_id},
        {"msgtype", "markdown"},
        {"agentid", cfg_.agent_id},
        {"markdown", {{"content", markdown}}},
    };
    try {
        auto resp = transport->post_json(
            "https://qyapi.weixin.qq.com/cgi-bin/message/send?access_token=" +
                access_token_,
            {{"Content-Type", "application/json"}},
            payload.dump());
        if (resp.status_code != 200) return false;
        auto body = nlohmann::json::parse(resp.body);
        return body.value("errcode", -1) == 0;
    } catch (...) {
        return false;
    }
}

bool WeComAdapter::send_textcard_via_api(const std::string& chat_id,
                                         const std::string& title,
                                         const std::string& description,
                                         const std::string& url) {
    if (access_token_.empty() && !refresh_access_token()) return false;
    auto* transport = get_transport();
    if (!transport) return false;
    nlohmann::json payload = {
        {"touser", chat_id},
        {"msgtype", "textcard"},
        {"agentid", cfg_.agent_id},
        {"textcard", {{"title", title}, {"description", description},
                      {"url", url}, {"btntxt", "Open"}}},
    };
    try {
        auto resp = transport->post_json(
            "https://qyapi.weixin.qq.com/cgi-bin/message/send?access_token=" +
                access_token_,
            {{"Content-Type", "application/json"}},
            payload.dump());
        if (resp.status_code != 200) return false;
        auto body = nlohmann::json::parse(resp.body);
        return body.value("errcode", -1) == 0;
    } catch (...) {
        return false;
    }
}

bool WeComAdapter::send_template_card_via_api(const std::string& chat_id,
                                              const nlohmann::json& card) {
    if (access_token_.empty() && !refresh_access_token()) return false;
    auto* transport = get_transport();
    if (!transport) return false;
    nlohmann::json payload = {
        {"touser", chat_id},
        {"msgtype", "template_card"},
        {"agentid", cfg_.agent_id},
        {"template_card", card},
    };
    try {
        auto resp = transport->post_json(
            "https://qyapi.weixin.qq.com/cgi-bin/message/send?access_token=" +
                access_token_,
            {{"Content-Type", "application/json"}},
            payload.dump());
        if (resp.status_code != 200) return false;
        auto body = nlohmann::json::parse(resp.body);
        return body.value("errcode", -1) == 0;
    } catch (...) {
        return false;
    }
}

bool WeComAdapter::send(const std::string& chat_id,
                        const std::string& content) {
    auto chunks = wecom_split_text(content, cfg_.max_message_length);
    bool any_fail = false;
    for (const auto& c : chunks) {
        bool ok = false;
        if (!cfg_.corp_id.empty() && !cfg_.agent_id.empty()) {
            ok = send_markdown_via_api(chat_id, c);
        } else if (!cfg_.webhook_url.empty()) {
            ok = send_webhook_message(c, "markdown");
        }
        if (!ok) any_fail = true;
    }
    return !any_fail;
}

void WeComAdapter::send_typing(const std::string& /*chat_id*/) {
    // No typing indicator in WeCom API.
}

// ----- WebSocket payload builders ------------------------------------------

nlohmann::json WeComAdapter::build_subscribe_payload() const {
    auto ts = std::chrono::duration_cast<std::chrono::seconds>(
                  std::chrono::system_clock::now().time_since_epoch())
                  .count();
    nlohmann::json p = {
        {"cmd", kWeComCmdSubscribe},
        {"req_id", new_req_id("sub")},
        {"bot_id", cfg_.bot_id},
        {"timestamp", ts},
    };
    if (!cfg_.secret.empty()) {
        // HMAC would be computed by the runner; we only emit the stub.
        p["secret_hint"] = std::string(cfg_.secret.size(), '*');
    }
    return p;
}

nlohmann::json WeComAdapter::build_send_payload(const std::string& chat_id,
                                                const std::string& content,
                                                const std::string& msgtype) const {
    return {
        {"cmd", kWeComCmdSend},
        {"req_id", new_req_id("send")},
        {"bot_id", cfg_.bot_id},
        {"chat_id", chat_id},
        {"msgtype", msgtype},
        {msgtype, {{"content", content}}},
    };
}

nlohmann::json WeComAdapter::build_ping_payload() {
    return {{"cmd", kWeComCmdPing}, {"req_id", new_req_id("ping")}};
}

nlohmann::json WeComAdapter::build_response_payload(const std::string& req_id,
                                                    const std::string& payload) {
    return {
        {"cmd", kWeComCmdRespond},
        {"req_id", req_id},
        {"bot_id", cfg_.bot_id},
        {"body", payload},
    };
}

std::string WeComAdapter::register_pending(const std::string& cmd,
                                           ResponseCallback cb,
                                           double timeout_seconds) {
    std::string rid = new_req_id(cmd);
    Pending p;
    p.cmd = cmd;
    p.cb = std::move(cb);
    p.expires = std::chrono::steady_clock::now() +
                std::chrono::milliseconds(static_cast<int>(timeout_seconds * 1000));
    std::lock_guard<std::mutex> lk(pending_mu_);
    pending_[rid] = std::move(p);
    return rid;
}

bool WeComAdapter::complete_pending(const std::string& req_id,
                                    const nlohmann::json& body, bool ok) {
    Pending p;
    {
        std::lock_guard<std::mutex> lk(pending_mu_);
        auto it = pending_.find(req_id);
        if (it == pending_.end()) return false;
        p = std::move(it->second);
        pending_.erase(it);
    }
    if (p.cb) p.cb(body, ok);
    return true;
}

std::size_t WeComAdapter::sweep_expired_pending() {
    auto now = std::chrono::steady_clock::now();
    std::vector<Pending> expired;
    {
        std::lock_guard<std::mutex> lk(pending_mu_);
        for (auto it = pending_.begin(); it != pending_.end();) {
            if (it->second.expires <= now) {
                expired.push_back(std::move(it->second));
                it = pending_.erase(it);
            } else {
                ++it;
            }
        }
    }
    for (auto& p : expired) {
        if (p.cb) p.cb(nlohmann::json::object({{"error", "timeout"}}), false);
    }
    return expired.size();
}

// ----- Dispatch & parsing ---------------------------------------------------

std::optional<MessageEvent> WeComAdapter::dispatch_payload(
    const nlohmann::json& payload, nlohmann::json* out_event) {
    if (!payload.is_object()) return std::nullopt;
    std::string cmd = payload.value("cmd", "");
    std::string req_id = payload.value("req_id", "");

    // Callback events — convert to MessageEvent.
    if (cmd == kWeComCmdCallback || cmd == kWeComCmdLegacyCallback) {
        nlohmann::json body = payload.value("body", nlohmann::json::object());
        auto [text, msg_id] = extract_text_body(body);
        if (!msg_id.empty() && !dedup_.check_and_add(msg_id)) {
            return std::nullopt;
        }
        std::string chat_id = body.value("chat_id", body.value("from_user_id", ""));
        std::string sender = body.value("from_user_id", body.value("user_id", ""));
        std::string chat_type = body.value("chat_type", "dm");
        if (chat_type == "group") {
            if (!is_group_allowed(chat_id, sender, text)) return std::nullopt;
        } else {
            if (!is_dm_allowed(sender)) return std::nullopt;
        }
        // Remember reply req_id so we can correlate the response.
        if (!msg_id.empty() && !req_id.empty()) {
            std::lock_guard<std::mutex> lk(reply_mu_);
            reply_req_ids_[msg_id] = req_id;
        }
        MessageEvent ev;
        ev.text = text;
        auto media = extract_media_refs(body);
        ev.message_type = wecom_derive_message_type(body, text, media);
        ev.source.platform = Platform::WeCom;
        ev.source.chat_id = chat_id;
        ev.source.chat_type = chat_type;
        ev.source.user_id = sender;
        ev.media_urls = std::move(media);
        return ev;
    }

    // Event callbacks (approval / menu-click / external-contact).
    if (cmd == kWeComCmdEventCallback) {
        if (out_event) *out_event = payload.value("body", nlohmann::json::object());
        return std::nullopt;
    }

    // Response to a previously-issued request.
    if (!req_id.empty()) {
        nlohmann::json body = payload.value("body", payload);
        bool ok = payload.value("errcode", 0) == 0;
        complete_pending(req_id, body, ok);
    }
    return std::nullopt;
}

std::pair<std::string, std::string> WeComAdapter::extract_text_body(
    const nlohmann::json& body) {
    std::string msg_id = body.value("msg_id", body.value("message_id", ""));
    std::string text;
    if (body.contains("text") && body["text"].is_object()) {
        text = body["text"].value("content", "");
    } else if (body.contains("content") && body["content"].is_string()) {
        text = body["content"].get<std::string>();
    } else if (body.contains("markdown") && body["markdown"].is_object()) {
        text = body["markdown"].value("content", "");
    }
    return {text, msg_id};
}

std::string WeComAdapter::extract_reply_req_id(const nlohmann::json& body) {
    if (body.contains("reply_req_id")) return body.value("reply_req_id", "");
    return body.value("req_id", "");
}

std::string WeComAdapter::classify_event(const nlohmann::json& payload) {
    std::string cmd = payload.value("cmd", "");
    if (cmd == kWeComCmdCallback || cmd == kWeComCmdLegacyCallback) return "message";
    if (cmd == kWeComCmdEventCallback) {
        nlohmann::json body = payload.value("body", nlohmann::json::object());
        std::string evt = body.value("event", "");
        if (evt.empty()) evt = body.value("event_type", "");
        if (!evt.empty()) return evt;
        return "event";
    }
    return cmd.empty() ? "unknown" : cmd;
}

std::vector<std::string> WeComAdapter::extract_media_refs(
    const nlohmann::json& body) const {
    std::vector<std::string> out;
    if (body.contains("media_id") && body["media_id"].is_string()) {
        out.push_back(body["media_id"].get<std::string>());
    }
    if (body.contains("image") && body["image"].is_object()) {
        std::string u = body["image"].value("url", body["image"].value("media_id", ""));
        if (!u.empty()) out.push_back(u);
    }
    if (body.contains("file") && body["file"].is_object()) {
        std::string u = body["file"].value("url", body["file"].value("media_id", ""));
        if (!u.empty()) out.push_back(u);
    }
    if (body.contains("voice") && body["voice"].is_object()) {
        std::string u = body["voice"].value("url", body["voice"].value("media_id", ""));
        if (!u.empty()) out.push_back(u);
    }
    if (body.contains("video") && body["video"].is_object()) {
        std::string u = body["video"].value("url", body["video"].value("media_id", ""));
        if (!u.empty()) out.push_back(u);
    }
    return out;
}

// ----- Upload planning ------------------------------------------------------

nlohmann::json WeComAdapter::build_upload_init(const WeComUploadSession& s) {
    return {
        {"cmd", kWeComCmdUploadInit},
        {"req_id", new_req_id("upinit")},
        {"bot_id", cfg_.bot_id},
        {"media_type", s.media_type},
        {"filename", s.filename},
        {"content_type", s.content_type},
        {"total_size", s.total_size},
    };
}

nlohmann::json WeComAdapter::build_upload_chunk(const WeComUploadSession& s,
                                                const std::string& chunk_bytes) {
    // chunk is base64-encoded.
    static const char tbl[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    int val = 0, valb = -6;
    for (unsigned char c : chunk_bytes) {
        val = (val << 8) | c;
        valb += 8;
        while (valb >= 0) {
            out.push_back(tbl[(val >> valb) & 0x3F]);
            valb -= 6;
        }
    }
    if (valb > -6) out.push_back(tbl[((val << 8) >> (valb + 8)) & 0x3F]);
    while (out.size() % 4) out.push_back('=');
    return {
        {"cmd", kWeComCmdUploadChunk},
        {"req_id", new_req_id("upchunk")},
        {"media_key", s.media_key},
        {"chunk_index", s.chunk_index},
        {"chunk_base64", out},
    };
}

nlohmann::json WeComAdapter::build_upload_finish(const WeComUploadSession& s) {
    return {
        {"cmd", kWeComCmdUploadFinish},
        {"req_id", new_req_id("upfin")},
        {"media_key", s.media_key},
        {"total_chunks", s.chunk_index},
    };
}

std::vector<nlohmann::json> WeComAdapter::plan_upload_payloads(
    const WeComUploadSession& s, const std::string& bytes) {
    std::vector<nlohmann::json> out;
    WeComUploadSession cur = s;
    cur.total_size = bytes.size();
    out.push_back(build_upload_init(cur));
    std::size_t off = 0;
    while (off < bytes.size()) {
        std::size_t take = std::min(kWeComUploadChunkSize, bytes.size() - off);
        cur.chunk_index += 1;
        cur.sent_bytes += take;
        out.push_back(build_upload_chunk(cur, bytes.substr(off, take)));
        off += take;
        if (cur.chunk_index >= kWeComMaxUploadChunks) break;
    }
    out.push_back(build_upload_finish(cur));
    return out;
}

// ----- Policy gating --------------------------------------------------------

WeComGroupRule WeComAdapter::resolve_group_rule(const std::string& chat_id) const {
    auto it = cfg_.groups.find(chat_id);
    if (it != cfg_.groups.end()) return it->second;
    WeComGroupRule r;
    r.policy = parse_wecom_group_policy(cfg_.group_policy);
    r.allow_from = cfg_.group_allow_from;
    return r;
}

bool WeComAdapter::is_dm_allowed(const std::string& sender_id) const {
    auto pol = parse_wecom_dm_policy(cfg_.dm_policy);
    if (pol == WeComDmPolicy::Disabled) return false;
    if (pol == WeComDmPolicy::Open) return true;
    if (pol == WeComDmPolicy::Pairing) {
        // Pairing policy: allowlist after pair token set; treat same as allowlist.
    }
    return wecom_entry_matches(cfg_.allow_from, sender_id);
}

bool WeComAdapter::is_group_allowed(const std::string& chat_id,
                                    const std::string& sender_id,
                                    const std::string& text) const {
    auto rule = resolve_group_rule(chat_id);
    if (rule.policy == WeComGroupPolicy::Disabled) return false;
    bool sender_allowed = rule.policy == WeComGroupPolicy::Open ||
                          wecom_entry_matches(rule.allow_from, sender_id);
    if (!sender_allowed) return false;
    if (!cfg_.require_group_mention) return true;
    std::string needle = "@" + cfg_.bot_name;
    return text.find(needle) != std::string::npos;
}

// ----- Misc ---------------------------------------------------------------

std::string WeComAdapter::new_req_id(const std::string& prefix) {
    static thread_local std::mt19937_64 rng{std::random_device{}()};
    std::uniform_int_distribution<uint64_t> dist;
    std::ostringstream oss;
    oss << prefix << '-' << std::hex << std::setw(16) << std::setfill('0')
        << dist(rng);
    return oss.str();
}

}  // namespace hermes::gateway::platforms
