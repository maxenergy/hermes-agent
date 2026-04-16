// Phase 12 — BlueBubbles (iMessage) platform adapter implementation (depth port).
//
// Mirrors gateway/platforms/bluebubbles.py — covers URL/credential plumbing,
// chat GUID resolution & cache, webhook registration/deregistration, outbound
// text/attachment/reaction builders, inbound webhook parsing, markdown
// stripping, and helpers (PII redaction, attachment classification).
#include "bluebubbles.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <fstream>
#include <iomanip>
#include <regex>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

namespace hermes::gateway::platforms {

namespace {

// Tapback associatedMessageType codes — keep in sync with Python adapter.
const std::unordered_set<int> kTapbackTypes{
    2000, 2001, 2002, 2003, 2004, 2005,
    3000, 3001, 3002, 3003, 3004, 3005,
};

// Webhook events that carry user messages.
const std::unordered_set<std::string> kMessageEvents{
    "new-message", "updated-message", "message"};

bool is_unreserved(unsigned char c) {
    return (std::isalnum(c) != 0) || c == '-' || c == '_' || c == '.' || c == '~';
}

std::string to_lower(std::string s) {
    for (auto& c : s) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return s;
}

std::string trim(std::string s) {
    auto not_space = [](unsigned char c) { return std::isspace(c) == 0; };
    s.erase(s.begin(), std::find_if(s.begin(), s.end(), not_space));
    s.erase(std::find_if(s.rbegin(), s.rend(), not_space).base(), s.end());
    return s;
}

bool starts_with_ci(const std::string& s, const std::string& prefix) {
    if (s.size() < prefix.size()) return false;
    for (std::size_t i = 0; i < prefix.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(s[i])) !=
            std::tolower(static_cast<unsigned char>(prefix[i]))) {
            return false;
        }
    }
    return true;
}

std::string first_str(const nlohmann::json& obj,
                      std::initializer_list<const char*> keys) {
    if (!obj.is_object()) return {};
    for (const char* k : keys) {
        auto it = obj.find(k);
        if (it != obj.end() && it->is_string()) {
            std::string v = trim(it->get<std::string>());
            if (!v.empty()) return v;
        }
    }
    return {};
}

}  // namespace

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

std::string bb_redact(const std::string& text) {
    static const std::regex kPhoneRe(R"(\+?\d{7,15})");
    static const std::regex kEmailRe(R"([\w.+-]+@[\w-]+\.[\w.]+)");
    std::string out = std::regex_replace(text, kPhoneRe, "[REDACTED]");
    out = std::regex_replace(out, kEmailRe, "[REDACTED]");
    return out;
}

std::string bb_normalize_server_url(const std::string& raw) {
    std::string value = trim(raw);
    if (value.empty()) return {};
    if (!starts_with_ci(value, "http://") && !starts_with_ci(value, "https://")) {
        value = "http://" + value;
    }
    while (!value.empty() && value.back() == '/') value.pop_back();
    return value;
}

std::string bb_strip_markdown(const std::string& text) {
    std::string s = text;
    // ** bold **
    s = std::regex_replace(s, std::regex(R"(\*\*([\s\S]+?)\*\*)"), "$1");
    // *italic*
    s = std::regex_replace(s, std::regex(R"(\*([\s\S]+?)\*)"), "$1");
    // __bold__
    s = std::regex_replace(s, std::regex(R"(__([\s\S]+?)__)"), "$1");
    // _italic_
    s = std::regex_replace(s, std::regex(R"(_([\s\S]+?)_)"), "$1");
    // ```fenced```
    s = std::regex_replace(s, std::regex(R"(```[a-zA-Z0-9_+\-]*\n?)"), "");
    // `code`
    s = std::regex_replace(s, std::regex(R"(`([^`]+?)`)"), "$1");
    // # heading
    s = std::regex_replace(s, std::regex(R"((^|\n)#{1,6}\s+)"), "$1");
    // [text](url)
    s = std::regex_replace(s, std::regex(R"(\[([^\]]+)\]\(([^\)]+)\))"), "$1");
    // collapse 3+ newlines
    s = std::regex_replace(s, std::regex(R"(\n{3,})"), "\n\n");
    return trim(s);
}

std::string bb_url_quote(const std::string& s) {
    std::ostringstream oss;
    oss << std::hex << std::uppercase << std::setfill('0');
    for (unsigned char c : s) {
        if (is_unreserved(c)) {
            oss << static_cast<char>(c);
        } else {
            oss << '%' << std::setw(2) << static_cast<int>(c);
        }
    }
    return oss.str();
}

bool bb_looks_like_handle(const std::string& s) {
    if (s.empty()) return false;
    if (s.find('@') != std::string::npos) return true;
    if (s.size() >= 2 && s[0] == '+' &&
        std::all_of(s.begin() + 1, s.end(), [](unsigned char c) {
            return std::isdigit(c) != 0;
        })) {
        return true;
    }
    return false;
}

bool bb_is_group_chat(const std::string& chat_id) {
    return chat_id.find(";+;") != std::string::npos;
}

std::string bb_classify_event_type(const nlohmann::json& payload) {
    std::string ev = first_str(payload, {"type", "event"});
    if (ev.empty()) return "unknown";
    if (kMessageEvents.count(ev)) return "message";
    if (ev == "typing-indicator" || ev == "typing") return "typing";
    if (ev == "read-status-update" || ev == "read") return "read";
    if (ev == "reaction-added" || ev == "reaction") return "reaction";
    return "unknown";
}

nlohmann::json bb_extract_attachments(const nlohmann::json& message) {
    nlohmann::json out = nlohmann::json::array();
    if (!message.is_object()) return out;
    auto it = message.find("attachments");
    if (it == message.end() || !it->is_array()) return out;
    for (const auto& att : *it) {
        if (!att.is_object()) continue;
        nlohmann::json item = {
            {"guid", att.value("guid", "")},
            {"mime_type", att.value("mimeType", "")},
            {"transfer_name", att.value("transferName", "")},
        };
        out.push_back(item);
    }
    return out;
}

std::string bb_normalize_reaction(const std::string& token) {
    std::string t = to_lower(trim(token));
    static const std::array<const char*, 7> kAllowed{
        "love", "like", "dislike", "laugh", "emphasize", "question", "exclamation"};
    for (const char* k : kAllowed) {
        if (t == k) return t;
    }
    if (t == "heart") return "love";
    if (t == "thumbsup" || t == "thumbs_up") return "like";
    if (t == "thumbsdown" || t == "thumbs_down") return "dislike";
    if (t == "haha") return "laugh";
    if (t == "exclaim" || t == "emphasis") return "emphasize";
    if (t == "?" || t == "question_mark") return "question";
    return {};
}

// ---------------------------------------------------------------------------
// Adapter
// ---------------------------------------------------------------------------

BlueBubblesAdapter::BlueBubblesAdapter(Config cfg) : cfg_(std::move(cfg)) {
    cfg_.server_url = bb_normalize_server_url(cfg_.server_url);
    if (!cfg_.webhook_path.empty() && cfg_.webhook_path.front() != '/') {
        cfg_.webhook_path = "/" + cfg_.webhook_path;
    }
}

BlueBubblesAdapter::BlueBubblesAdapter(Config cfg, hermes::llm::HttpTransport* transport)
    : BlueBubblesAdapter(std::move(cfg)) {
    transport_ = transport;
}

hermes::llm::HttpTransport* BlueBubblesAdapter::get_transport() {
    if (transport_) return transport_;
    return hermes::llm::get_default_transport();
}

std::string BlueBubblesAdapter::api_url(const std::string& path) const {
    char sep = (path.find('?') == std::string::npos) ? '?' : '&';
    return cfg_.server_url + path + sep + "password=" +
           bb_url_quote(cfg_.password);
}

std::string BlueBubblesAdapter::webhook_external_url() const {
    std::string host = cfg_.webhook_host;
    if (host == "0.0.0.0" || host == "127.0.0.1" || host == "localhost" || host == "::") {
        host = "localhost";
    }
    std::ostringstream oss;
    oss << "http://" << host << ":" << cfg_.webhook_port << cfg_.webhook_path;
    return oss.str();
}

bool BlueBubblesAdapter::probe_server() {
    auto* t = get_transport();
    if (!t) return false;
    try {
        auto info = t->get(api_url("/api/v1/server/info"), {});
        if (info.status_code != 200) return false;
        auto js = nlohmann::json::parse(info.body, nullptr, false);
        if (js.is_discarded()) return false;
        if (auto data = js.find("data"); data != js.end() && data->is_object()) {
            private_api_enabled_ = data->value("private_api", false);
            helper_connected_ = data->value("helper_connected", false);
        }
        return true;
    } catch (...) {
        last_error_kind_ = AdapterErrorKind::Retryable;
        return false;
    }
}

bool BlueBubblesAdapter::connect() {
    if (cfg_.server_url.empty() || cfg_.password.empty()) {
        last_error_kind_ = AdapterErrorKind::Fatal;
        return false;
    }
    if (!probe_server()) return false;
    register_webhook();
    connected_ = true;
    last_error_kind_ = AdapterErrorKind::None;
    return true;
}

void BlueBubblesAdapter::disconnect() {
    if (connected_) {
        unregister_webhook();
    }
    connected_ = false;
}

nlohmann::json BlueBubblesAdapter::api_get(const std::string& path) {
    auto* t = get_transport();
    if (!t) return {};
    auto resp = t->get(api_url(path), {});
    if (resp.status_code < 200 || resp.status_code >= 300) return {};
    auto js = nlohmann::json::parse(resp.body, nullptr, false);
    return js.is_discarded() ? nlohmann::json{} : js;
}

nlohmann::json BlueBubblesAdapter::api_post(const std::string& path,
                                            const nlohmann::json& payload) {
    auto* t = get_transport();
    if (!t) return {};
    auto resp = t->post_json(api_url(path),
                             {{"Content-Type", "application/json"}},
                             payload.dump());
    if (resp.status_code < 200 || resp.status_code >= 300) return {};
    auto js = nlohmann::json::parse(resp.body, nullptr, false);
    return js.is_discarded() ? nlohmann::json{} : js;
}

nlohmann::json BlueBubblesAdapter::api_delete(const std::string& path) {
    // HttpTransport doesn't expose DELETE; fall back to a POST with override.
    auto* t = get_transport();
    if (!t) return {};
    auto resp = t->post_json(
        api_url(path),
        {{"Content-Type", "application/json"},
         {"X-HTTP-Method-Override", "DELETE"}},
        "{}");
    if (resp.status_code < 200 || resp.status_code >= 300) return {};
    auto js = nlohmann::json::parse(resp.body, nullptr, false);
    return js.is_discarded() ? nlohmann::json{} : js;
}

std::vector<nlohmann::json> BlueBubblesAdapter::find_registered_webhooks(
    const std::string& url) {
    std::vector<nlohmann::json> out;
    auto js = api_get("/api/v1/webhook");
    if (!js.is_object()) return out;
    auto data = js.find("data");
    if (data == js.end() || !data->is_array()) return out;
    for (const auto& wh : *data) {
        if (wh.is_object() && wh.value("url", "") == url) out.push_back(wh);
    }
    return out;
}

bool BlueBubblesAdapter::register_webhook() {
    std::string url = webhook_external_url();
    auto existing = find_registered_webhooks(url);
    if (!existing.empty()) return true;
    nlohmann::json payload = {
        {"url", url},
        {"events", {"new-message", "updated-message", "message"}},
    };
    auto res = api_post("/api/v1/webhook", payload);
    if (!res.is_object()) return false;
    int status = res.value("status", 0);
    return status >= 200 && status < 300;
}

bool BlueBubblesAdapter::unregister_webhook() {
    std::string url = webhook_external_url();
    bool removed = false;
    for (const auto& wh : find_registered_webhooks(url)) {
        std::string id = wh.value("id", "");
        if (id.empty() && wh.contains("id") && wh["id"].is_number_integer()) {
            id = std::to_string(wh["id"].get<long long>());
        }
        if (id.empty()) continue;
        api_delete("/api/v1/webhook/" + bb_url_quote(id));
        removed = true;
    }
    return removed;
}

std::optional<std::string> BlueBubblesAdapter::resolve_chat_guid(
    const std::string& target) {
    std::string t = trim(target);
    if (t.empty()) return std::nullopt;
    if (t.find(';') != std::string::npos) return t;
    {
        std::lock_guard<std::mutex> lock(guid_mu_);
        auto it = guid_cache_.find(t);
        if (it != guid_cache_.end()) return it->second;
    }
    nlohmann::json payload = {
        {"limit", 100}, {"offset", 0}, {"with", {"participants"}}};
    auto res = api_post("/api/v1/chat/query", payload);
    if (!res.is_object()) return std::nullopt;
    auto data = res.find("data");
    if (data == res.end() || !data->is_array()) return std::nullopt;
    for (const auto& chat : *data) {
        if (!chat.is_object()) continue;
        std::string guid = first_str(chat, {"guid", "chatGuid"});
        std::string identifier = first_str(chat, {"chatIdentifier", "identifier"});
        if (identifier == t && !guid.empty()) {
            std::lock_guard<std::mutex> lock(guid_mu_);
            guid_cache_[t] = guid;
            return guid;
        }
        auto parts = chat.find("participants");
        if (parts != chat.end() && parts->is_array()) {
            for (const auto& p : *parts) {
                std::string addr = first_str(p, {"address"});
                if (addr == t && !guid.empty()) {
                    std::lock_guard<std::mutex> lock(guid_mu_);
                    guid_cache_[t] = guid;
                    return guid;
                }
            }
        }
    }
    return std::nullopt;
}

std::optional<std::string> BlueBubblesAdapter::create_chat_for_handle(
    const std::string& handle, const std::string& text) {
    nlohmann::json payload = {
        {"addresses", {handle}},
        {"message", text},
        {"tempGuid", BlueBubblesAdapter::temp_guid_for(
                         std::chrono::system_clock::now())},
    };
    auto res = api_post("/api/v1/chat/new", payload);
    if (!res.is_object()) return std::nullopt;
    auto data = res.find("data");
    if (data == res.end() || !data->is_object()) return std::nullopt;
    std::string guid = first_str(*data, {"guid", "chatGuid", "messageGuid"});
    if (guid.empty()) return std::nullopt;
    return guid;
}

std::string BlueBubblesAdapter::temp_guid_for(
    std::chrono::system_clock::time_point now) {
    auto epoch = now.time_since_epoch();
    auto secs = std::chrono::duration_cast<std::chrono::milliseconds>(epoch).count();
    std::ostringstream oss;
    oss << "temp-" << (static_cast<double>(secs) / 1000.0);
    return oss.str();
}

nlohmann::json BlueBubblesAdapter::build_text_payload(
    const std::string& chat_guid, const std::string& text,
    const std::optional<std::string>& reply_to) const {
    nlohmann::json payload = {
        {"chatGuid", chat_guid},
        {"tempGuid", BlueBubblesAdapter::temp_guid_for(
                         std::chrono::system_clock::now())},
        {"message", text},
    };
    if (reply_to && private_api_enabled_ && helper_connected_) {
        payload["method"] = "private-api";
        payload["selectedMessageGuid"] = *reply_to;
        payload["partIndex"] = 0;
    }
    return payload;
}

BlueBubblesAdapter::SendResult BlueBubblesAdapter::send_text(
    const std::string& chat_id, const std::string& text,
    const std::optional<std::string>& reply_to) {
    SendResult sr;
    std::string body = bb_strip_markdown(text);
    if (body.empty()) {
        sr.error = "BlueBubbles send requires text";
        return sr;
    }
    auto guid = resolve_chat_guid(chat_id);
    if (!guid) {
        if (private_api_enabled_ && bb_looks_like_handle(chat_id)) {
            auto created = create_chat_for_handle(chat_id, body);
            if (created) {
                sr.success = true;
                sr.message_id = *created;
                return sr;
            }
        }
        sr.error = "BlueBubbles chat not found for target: " + chat_id;
        return sr;
    }
    auto payload = build_text_payload(*guid, body, reply_to);
    auto res = api_post("/api/v1/message/text", payload);
    if (!res.is_object()) {
        sr.error = "POST /message/text failed";
        return sr;
    }
    auto data = res.find("data");
    if (data != res.end() && data->is_object()) {
        sr.message_id = first_str(*data, {"guid", "messageGuid"});
        if (sr.message_id.empty()) sr.message_id = "ok";
    }
    sr.success = true;
    sr.raw = res;
    return sr;
}

BlueBubblesAdapter::SendResult BlueBubblesAdapter::send_attachment(
    const std::string& chat_id, const std::string& file_path,
    const std::string& filename, const std::string& caption,
    bool is_audio_message) {
    SendResult sr;
    auto guid = resolve_chat_guid(chat_id);
    if (!guid) {
        sr.error = "Chat not found: " + chat_id;
        return sr;
    }
    auto* t = get_transport();
    if (!t) {
        sr.error = "HTTP transport unavailable";
        return sr;
    }
    // Read the file into memory.  BlueBubbles attachment endpoint caps at
    // ~25 MiB for iMessage; oversized uploads will be rejected server-side.
    std::ifstream in(file_path, std::ios::binary);
    if (!in) {
        sr.error = "Cannot open file: " + file_path;
        return sr;
    }
    std::ostringstream buf;
    buf << in.rdbuf();
    std::string file_bytes = buf.str();

    std::string effective_name = filename;
    if (effective_name.empty()) {
        auto pos = file_path.find_last_of("/\\");
        effective_name = (pos == std::string::npos) ? file_path
                                                    : file_path.substr(pos + 1);
    }

    // Build multipart/form-data body.  Boundary is derived from the temp
    // GUID so it can never appear in the binary payload.
    const std::string temp_guid =
        BlueBubblesAdapter::temp_guid_for(std::chrono::system_clock::now());
    const std::string boundary = "----hermes-bb-" + temp_guid;
    const std::string crlf = "\r\n";

    auto add_field = [&](std::ostringstream& o, const std::string& name,
                         const std::string& value) {
        o << "--" << boundary << crlf
          << "Content-Disposition: form-data; name=\"" << name << "\""
          << crlf << crlf
          << value << crlf;
    };

    std::ostringstream body;
    add_field(body, "chatGuid", *guid);
    add_field(body, "tempGuid", temp_guid);
    add_field(body, "name", effective_name);
    if (is_audio_message) add_field(body, "isAudioMessage", "1");
    if (!caption.empty()) add_field(body, "message", caption);
    body << "--" << boundary << crlf
         << "Content-Disposition: form-data; name=\"attachment\"; filename=\""
         << effective_name << "\"" << crlf
         << "Content-Type: application/octet-stream" << crlf << crlf;
    body.write(file_bytes.data(),
               static_cast<std::streamsize>(file_bytes.size()));
    body << crlf << "--" << boundary << "--" << crlf;

    std::unordered_map<std::string, std::string> headers = {
        {"Content-Type", "multipart/form-data; boundary=" + boundary},
    };
    auto resp = t->post_json(api_url("/api/v1/message/attachment"),
                             headers, body.str());
    if (resp.status_code < 200 || resp.status_code >= 300) {
        sr.error = "upload failed: HTTP " + std::to_string(resp.status_code);
        return sr;
    }
    auto js = nlohmann::json::parse(resp.body, nullptr, false);
    if (!js.is_discarded() && js.is_object()) {
        if (auto d = js.find("data"); d != js.end() && d->is_object()) {
            sr.message_id = first_str(*d, {"guid", "messageGuid"});
        }
        sr.raw = js;
    }
    if (sr.message_id.empty()) sr.message_id = temp_guid;
    sr.success = true;
    return sr;
}

BlueBubblesAdapter::SendResult BlueBubblesAdapter::send_reaction(
    const std::string& chat_id, const std::string& message_guid,
    const std::string& reaction, int part_index) {
    SendResult sr;
    if (!private_api_enabled_ || !helper_connected_) {
        sr.error = "Private API helper not connected";
        return sr;
    }
    std::string norm = bb_normalize_reaction(reaction);
    if (norm.empty()) {
        sr.error = "Unsupported reaction: " + reaction;
        return sr;
    }
    auto guid = resolve_chat_guid(chat_id);
    if (!guid) {
        sr.error = "Chat not found: " + chat_id;
        return sr;
    }
    nlohmann::json payload = {
        {"chatGuid", *guid},
        {"selectedMessageGuid", message_guid},
        {"reaction", norm},
        {"partIndex", part_index},
    };
    auto res = api_post("/api/v1/message/react", payload);
    sr.success = res.is_object();
    sr.raw = res;
    return sr;
}

bool BlueBubblesAdapter::send(const std::string& chat_id,
                              const std::string& content) {
    return send_text(chat_id, content).success;
}

void BlueBubblesAdapter::send_typing(const std::string& chat_id) {
    if (!private_api_enabled_ || !helper_connected_) return;
    auto guid = resolve_chat_guid(chat_id);
    if (!guid) return;
    api_post("/api/v1/chat/" + bb_url_quote(*guid) + "/typing", nlohmann::json::object());
}

bool BlueBubblesAdapter::stop_typing(const std::string& chat_id) {
    if (!private_api_enabled_ || !helper_connected_) return false;
    auto guid = resolve_chat_guid(chat_id);
    if (!guid) return false;
    api_delete("/api/v1/chat/" + bb_url_quote(*guid) + "/typing");
    return true;
}

bool BlueBubblesAdapter::mark_read(const std::string& chat_id) {
    if (!cfg_.send_read_receipts) return false;
    if (!private_api_enabled_ || !helper_connected_) return false;
    auto guid = resolve_chat_guid(chat_id);
    if (!guid) return false;
    api_post("/api/v1/chat/" + bb_url_quote(*guid) + "/read", nlohmann::json::object());
    return true;
}

nlohmann::json BlueBubblesAdapter::get_chat_info(const std::string& chat_id) {
    nlohmann::json info = {
        {"name", chat_id},
        {"type", bb_is_group_chat(chat_id) ? "group" : "dm"},
    };
    auto guid = resolve_chat_guid(chat_id);
    if (!guid) return info;
    auto res = api_get("/api/v1/chat/" + bb_url_quote(*guid) +
                       "?with=participants");
    if (!res.is_object()) return info;
    auto data = res.find("data");
    if (data == res.end() || !data->is_object()) return info;
    std::string display = first_str(*data, {"displayName", "chatIdentifier"});
    if (!display.empty()) info["name"] = display;
    auto parts = data->find("participants");
    if (parts != data->end() && parts->is_array()) {
        std::vector<std::string> participants;
        for (const auto& p : *parts) {
            std::string addr = first_str(p, {"address"});
            if (!addr.empty()) participants.push_back(addr);
        }
        if (!participants.empty()) info["participants"] = participants;
    }
    return info;
}

std::optional<MessageEvent> BlueBubblesAdapter::parse_webhook_body(
    const nlohmann::json& payload) {
    if (!payload.is_object()) return std::nullopt;
    std::string ev_class = bb_classify_event_type(payload);
    if (ev_class != "message") return std::nullopt;

    // Pull the record ("data" or message field).
    nlohmann::json record;
    auto data = payload.find("data");
    if (data != payload.end()) {
        if (data->is_object()) record = *data;
        else if (data->is_array() && !data->empty() && (*data)[0].is_object()) {
            record = (*data)[0];
        }
    }
    if (record.is_null()) {
        auto msg = payload.find("message");
        if (msg != payload.end() && msg->is_object()) record = *msg;
    }
    if (record.is_null()) record = payload;

    bool is_from_me = record.value("isFromMe", false) ||
                      record.value("fromMe", false) ||
                      record.value("is_from_me", false);
    if (is_from_me) return std::nullopt;

    if (auto at = record.find("associatedMessageType");
        at != record.end() && at->is_number_integer() &&
        kTapbackTypes.count(at->get<int>())) {
        return std::nullopt;  // tapback delivered as message
    }

    std::string text = first_str(record, {"text", "message", "body"});

    std::string chat_guid = first_str(record, {"chatGuid", "chat_guid"});
    if (chat_guid.empty()) chat_guid = first_str(payload, {"chatGuid", "chat_guid", "guid"});
    std::string chat_identifier = first_str(record, {"chatIdentifier", "identifier"});
    if (chat_identifier.empty()) {
        chat_identifier = first_str(payload, {"chatIdentifier", "identifier"});
    }
    std::string sender;
    auto handle = record.find("handle");
    if (handle != record.end() && handle->is_object()) {
        sender = first_str(*handle, {"address"});
    }
    if (sender.empty()) sender = first_str(record, {"sender", "from", "address"});
    if (sender.empty()) sender = !chat_identifier.empty() ? chat_identifier : chat_guid;

    if (chat_guid.empty() && chat_identifier.empty() && !sender.empty()) {
        chat_identifier = sender;
    }

    std::string session_chat = !chat_guid.empty() ? chat_guid : chat_identifier;
    if (sender.empty() || session_chat.empty()) return std::nullopt;

    auto attachments = bb_extract_attachments(record);
    bool has_image = false, has_audio = false, has_video = false;
    std::vector<std::string> media_urls;
    for (const auto& a : attachments) {
        std::string mime = a.value("mime_type", "");
        std::string m = to_lower(mime);
        media_urls.push_back(a.value("guid", ""));
        if (m.rfind("image/", 0) == 0) has_image = true;
        else if (m.rfind("audio/", 0) == 0) has_audio = true;
        else if (m.rfind("video/", 0) == 0) has_video = true;
    }

    std::string mtype = "TEXT";
    if (!attachments.empty()) {
        if (has_image) mtype = "PHOTO";
        else if (has_video) mtype = "VIDEO";
        else if (has_audio) mtype = "VOICE";
        else mtype = "DOCUMENT";
    }
    if (text.empty() && !media_urls.empty()) text = "(attachment)";
    if (text.empty()) return std::nullopt;

    MessageEvent ev;
    ev.text = text;
    ev.message_type = mtype;
    ev.source.platform = Platform::BlueBubbles;
    ev.source.chat_id = session_chat;
    ev.source.chat_name = !chat_identifier.empty() ? chat_identifier : sender;
    ev.source.chat_type = bb_is_group_chat(chat_guid) ||
                                  record.value("isGroup", false)
                              ? "group"
                              : "dm";
    ev.source.user_id = sender;
    ev.source.user_name = sender;
    if (!chat_identifier.empty()) ev.source.chat_id_alt = chat_identifier;
    ev.media_urls = std::move(media_urls);
    std::string mid = first_str(record, {"guid", "messageGuid", "id"});
    std::string reply = first_str(record, {"threadOriginatorGuid", "associatedMessageGuid"});
    if (!reply.empty()) ev.reply_to_message_id = reply;
    (void)mid;  // MessageEvent has no message_id field; mid kept for parity.
    return ev;
}

std::string BlueBubblesAdapter::download_attachment(const std::string& att_guid) {
    auto* t = get_transport();
    if (!t) return {};
    auto resp = t->get(api_url("/api/v1/attachment/" + bb_url_quote(att_guid) +
                               "/download"),
                       {});
    if (resp.status_code < 200 || resp.status_code >= 300) return {};
    return resp.body;
}

std::size_t BlueBubblesAdapter::guid_cache_size() const {
    std::lock_guard<std::mutex> lock(guid_mu_);
    return guid_cache_.size();
}

void BlueBubblesAdapter::clear_guid_cache() {
    std::lock_guard<std::mutex> lock(guid_mu_);
    guid_cache_.clear();
}

}  // namespace hermes::gateway::platforms
