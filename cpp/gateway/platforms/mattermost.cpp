// Phase 12 — Mattermost platform adapter implementation (depth port).
#include "mattermost.hpp"

#include <algorithm>
#include <cctype>
#include <regex>
#include <sstream>

namespace hermes::gateway::platforms {

namespace {

std::string to_lower(std::string s) {
    for (auto& c : s) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
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

std::string strip_prefix_slash(const std::string& path) {
    std::size_t i = 0;
    while (i < path.size() && path[i] == '/') ++i;
    return path.substr(i);
}

std::string trim_trailing_slash(std::string s) {
    while (!s.empty() && s.back() == '/') s.pop_back();
    return s;
}

}  // namespace

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

std::string mm_channel_type_to_chat_type(const std::string& code) {
    if (code == "D") return "dm";
    if (code == "G") return "group";
    if (code == "P") return "group";
    return "channel";
}

std::string mm_websocket_url(const std::string& base_url) {
    std::string s = trim_trailing_slash(base_url);
    if (starts_with_ci(s, "https://")) {
        return "wss://" + s.substr(8) + "/api/v4/websocket";
    }
    if (starts_with_ci(s, "http://")) {
        return "ws://" + s.substr(7) + "/api/v4/websocket";
    }
    return "ws://" + s + "/api/v4/websocket";
}

std::string mm_strip_image_markdown(const std::string& content) {
    static const std::regex re(R"(!\[([^\]]*)\]\(([^)]+)\))");
    return std::regex_replace(content, re, "$2");
}

nlohmann::json mm_build_auth_challenge(const std::string& token, int seq) {
    return nlohmann::json{
        {"seq", seq},
        {"action", "authentication_challenge"},
        {"data", {{"token", token}}},
    };
}

nlohmann::json mm_build_post_payload(const std::string& channel_id,
                                     const std::string& message,
                                     const std::string& root_id) {
    nlohmann::json payload = {
        {"channel_id", channel_id},
        {"message", message},
    };
    if (!root_id.empty()) payload["root_id"] = root_id;
    return payload;
}

nlohmann::json mm_build_post_with_files_payload(
    const std::string& channel_id, const std::string& caption,
    const std::vector<std::string>& file_ids, const std::string& root_id) {
    nlohmann::json payload = {
        {"channel_id", channel_id},
        {"message", caption},
        {"file_ids", file_ids},
    };
    if (!root_id.empty()) payload["root_id"] = root_id;
    return payload;
}

bool mm_message_mentions_bot(const std::string& message,
                             const std::string& bot_user_id,
                             const std::string& bot_username) {
    std::string m = to_lower(message);
    if (!bot_username.empty()) {
        std::string p = to_lower("@" + bot_username);
        if (m.find(p) != std::string::npos) return true;
    }
    if (!bot_user_id.empty()) {
        std::string p = to_lower("@" + bot_user_id);
        if (m.find(p) != std::string::npos) return true;
    }
    return false;
}

std::string mm_strip_mentions(const std::string& message,
                              const std::string& bot_user_id,
                              const std::string& bot_username) {
    std::string out = message;
    auto strip_token = [&](const std::string& token) {
        if (token.size() <= 1) return;
        // Case-insensitive replace of all occurrences.
        std::string lower_tok = to_lower(token);
        std::string lower = to_lower(out);
        std::string result;
        result.reserve(out.size());
        std::size_t i = 0;
        while (i < out.size()) {
            if (i + lower_tok.size() <= lower.size() &&
                lower.compare(i, lower_tok.size(), lower_tok) == 0) {
                i += lower_tok.size();
                continue;
            }
            result.push_back(out[i]);
            ++i;
        }
        out = result;
    };
    if (!bot_username.empty()) strip_token("@" + bot_username);
    if (!bot_user_id.empty()) strip_token("@" + bot_user_id);
    // Trim leading/trailing whitespace.
    auto not_space = [](unsigned char c) { return std::isspace(c) == 0; };
    out.erase(out.begin(), std::find_if(out.begin(), out.end(), not_space));
    out.erase(std::find_if(out.rbegin(), out.rend(), not_space).base(), out.end());
    return out;
}

MmPostedEvent mm_parse_posted_event(const nlohmann::json& event) {
    MmPostedEvent out;
    if (!event.is_object()) return out;
    out.event_type = event.value("event", "");
    auto data = event.find("data");
    if (data == event.end() || !data->is_object()) return out;
    out.channel_type_raw = data->value("channel_type", "O");
    out.sender_name = data->value("sender_name", "");
    if (!out.sender_name.empty() && out.sender_name.front() == '@') {
        out.sender_name = out.sender_name.substr(1);
    }
    auto post_str = data->find("post");
    if (post_str == data->end() || !post_str->is_string()) return out;
    auto parsed = nlohmann::json::parse(post_str->get<std::string>(), nullptr,
                                        false);
    if (parsed.is_discarded() || !parsed.is_object()) return out;
    out.post = std::move(parsed);
    out.valid = true;
    return out;
}

std::string mm_classify_message_type(const std::string& text,
                                     const std::vector<std::string>& mime_types) {
    if (!text.empty() && text.front() == '/') return "COMMAND";
    if (mime_types.empty()) return "TEXT";
    for (const auto& m : mime_types) {
        if (m.rfind("image/", 0) == 0) return "PHOTO";
    }
    for (const auto& m : mime_types) {
        if (m.rfind("audio/", 0) == 0) return "VOICE";
    }
    return "DOCUMENT";
}

// ---------------------------------------------------------------------------
// Adapter
// ---------------------------------------------------------------------------

MattermostAdapter::MattermostAdapter(Config cfg) : cfg_(std::move(cfg)) {
    cfg_.url = trim_trailing_slash(cfg_.url);
    cfg_.reply_mode = to_lower(cfg_.reply_mode);
}

MattermostAdapter::MattermostAdapter(Config cfg, hermes::llm::HttpTransport* transport)
    : MattermostAdapter(std::move(cfg)) {
    transport_ = transport;
}

hermes::llm::HttpTransport* MattermostAdapter::get_transport() {
    if (transport_) return transport_;
    return hermes::llm::get_default_transport();
}

std::unordered_map<std::string, std::string>
MattermostAdapter::auth_headers() const {
    return {{"Authorization", "Bearer " + cfg_.token},
            {"Content-Type", "application/json"}};
}

bool MattermostAdapter::connect() {
    if (cfg_.token.empty() || cfg_.url.empty()) {
        last_error_kind_ = AdapterErrorKind::Fatal;
        return false;
    }
    auto* t = get_transport();
    if (!t) {
        last_error_kind_ = AdapterErrorKind::Retryable;
        return false;
    }
    try {
        auto resp = t->get(cfg_.url + "/api/v4/users/me", auth_headers());
        if (resp.status_code == 401 || resp.status_code == 403) {
            last_error_kind_ = AdapterErrorKind::Fatal;
            return false;
        }
        if (resp.status_code != 200) {
            last_error_kind_ = AdapterErrorKind::Retryable;
            return false;
        }
        auto js = nlohmann::json::parse(resp.body, nullptr, false);
        if (!js.is_discarded() && js.is_object()) {
            bot_user_id_ = js.value("id", "");
            bot_username_ = js.value("username", "");
        }
        connected_ = true;
        last_error_kind_ = AdapterErrorKind::None;
        return true;
    } catch (...) {
        last_error_kind_ = AdapterErrorKind::Retryable;
        return false;
    }
}

void MattermostAdapter::disconnect() {
    connected_ = false;
}

nlohmann::json MattermostAdapter::api_get(const std::string& path) {
    auto* t = get_transport();
    if (!t) return {};
    auto url = cfg_.url + "/api/v4/" + strip_prefix_slash(path);
    auto resp = t->get(url, auth_headers());
    if (resp.status_code >= 400) return {};
    auto js = nlohmann::json::parse(resp.body, nullptr, false);
    return js.is_discarded() ? nlohmann::json{} : js;
}

nlohmann::json MattermostAdapter::api_post(const std::string& path,
                                           const nlohmann::json& payload) {
    auto* t = get_transport();
    if (!t) return {};
    auto url = cfg_.url + "/api/v4/" + strip_prefix_slash(path);
    auto resp = t->post_json(url, auth_headers(), payload.dump());
    if (resp.status_code >= 400) return {};
    auto js = nlohmann::json::parse(resp.body, nullptr, false);
    return js.is_discarded() ? nlohmann::json{} : js;
}

nlohmann::json MattermostAdapter::api_put(const std::string& path,
                                          const nlohmann::json& payload) {
    auto* t = get_transport();
    if (!t) return {};
    auto url = cfg_.url + "/api/v4/" + strip_prefix_slash(path);
    auto headers = auth_headers();
    headers["X-HTTP-Method-Override"] = "PUT";
    auto resp = t->post_json(url, headers, payload.dump());
    if (resp.status_code >= 400) return {};
    auto js = nlohmann::json::parse(resp.body, nullptr, false);
    return js.is_discarded() ? nlohmann::json{} : js;
}

std::string MattermostAdapter::create_post(const std::string& channel_id,
                                           const std::string& message,
                                           const std::string& reply_to) {
    std::string root_id;
    if (!reply_to.empty() && cfg_.reply_mode == "thread") root_id = reply_to;
    auto formatted = mm_strip_image_markdown(message);
    // Truncate / chunk to MAX_POST_LENGTH.
    std::vector<std::string> chunks;
    if (formatted.size() <= kMmMaxPostLength) {
        chunks.push_back(formatted);
    } else {
        for (std::size_t i = 0; i < formatted.size(); i += kMmMaxPostLength) {
            chunks.push_back(formatted.substr(i, kMmMaxPostLength));
        }
    }
    std::string last_id;
    for (const auto& chunk : chunks) {
        auto resp = api_post("posts",
                             mm_build_post_payload(channel_id, chunk, root_id));
        if (!resp.is_object() || !resp.contains("id")) return {};
        last_id = resp.value("id", "");
    }
    return last_id;
}

bool MattermostAdapter::edit_post(const std::string& post_id,
                                  const std::string& message) {
    auto resp = api_put("posts/" + post_id + "/patch",
                        nlohmann::json{{"message", message}});
    return resp.is_object() && resp.contains("id");
}

nlohmann::json MattermostAdapter::get_chat_info(const std::string& channel_id) {
    auto data = api_get("channels/" + channel_id);
    nlohmann::json out = {{"name", channel_id}, {"type", "channel"}};
    if (!data.is_object()) return out;
    std::string code = data.value("type", "O");
    out["type"] = mm_channel_type_to_chat_type(code);
    std::string display = data.value("display_name", "");
    if (display.empty()) display = data.value("name", channel_id);
    out["name"] = display;
    return out;
}

bool MattermostAdapter::send(const std::string& chat_id,
                             const std::string& content) {
    return !create_post(chat_id, content, "").empty();
}

void MattermostAdapter::send_typing(const std::string& chat_id) {
    if (bot_user_id_.empty()) return;
    api_post("users/" + bot_user_id_ + "/typing",
             nlohmann::json{{"channel_id", chat_id}});
}

bool MattermostAdapter::seen_post(const std::string& post_id) {
    std::lock_guard<std::mutex> lock(seen_mu_);
    return seen_posts_.count(post_id) > 0;
}

void MattermostAdapter::mark_seen(const std::string& post_id) {
    std::lock_guard<std::mutex> lock(seen_mu_);
    seen_posts_[post_id] = std::chrono::system_clock::now();
}

void MattermostAdapter::prune_seen() {
    std::lock_guard<std::mutex> lock(seen_mu_);
    if (seen_posts_.size() < kMmDedupMax) return;
    auto now = std::chrono::system_clock::now();
    for (auto it = seen_posts_.begin(); it != seen_posts_.end();) {
        if (now - it->second > kMmDedupTtl) it = seen_posts_.erase(it);
        else ++it;
    }
}

std::size_t MattermostAdapter::seen_size() const {
    std::lock_guard<std::mutex> lock(seen_mu_);
    return seen_posts_.size();
}

void MattermostAdapter::set_bot_identity(std::string user_id,
                                         std::string username) {
    bot_user_id_ = std::move(user_id);
    bot_username_ = std::move(username);
}

}  // namespace hermes::gateway::platforms
