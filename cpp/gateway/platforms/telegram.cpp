// Phase 12 — Telegram platform adapter implementation.
//
// Full-depth C++17 port of gateway/platforms/telegram.py.  The Python adapter
// uses python-telegram-bot on asyncio; here we talk directly to the Bot API
// via the injected HttpTransport, which lets us unit-test every code path
// with FakeHttpTransport (same pattern as the Discord/Slack adapters).
#include "telegram.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <regex>
#include <sstream>
#include <string>
#include <thread>
#include <utility>

#include <nlohmann/json.hpp>

#include <hermes/gateway/status.hpp>

namespace hermes::gateway::platforms {

// ---------------------------------------------------------------------------
// Small utilities
// ---------------------------------------------------------------------------

namespace {

std::string to_lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    return s;
}

bool contains_ci(const std::string& hay, const std::string& needle) {
    if (needle.empty()) return true;
    auto it = std::search(hay.begin(), hay.end(), needle.begin(), needle.end(),
                          [](char a, char b) {
                              return std::tolower(static_cast<unsigned char>(a)) ==
                                     std::tolower(static_cast<unsigned char>(b));
                          });
    return it != hay.end();
}

std::string trim(const std::string& s) {
    auto start = s.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return {};
    auto end = s.find_last_not_of(" \t\r\n");
    return s.substr(start, end - start + 1);
}

std::string cap_caption(const std::string& s) {
    if (s.size() <= TelegramAdapter::kCaptionMaxLength) return s;
    return s.substr(0, TelegramAdapter::kCaptionMaxLength);
}

}  // namespace

// ---------------------------------------------------------------------------
// Chat type
// ---------------------------------------------------------------------------

TelegramChatType parse_chat_type(const std::string& raw) {
    auto r = to_lower(raw);
    if (r == "private") return TelegramChatType::Private;
    if (r == "group") return TelegramChatType::Group;
    if (r == "supergroup") return TelegramChatType::Supergroup;
    if (r == "channel") return TelegramChatType::Channel;
    return TelegramChatType::Unknown;
}

std::string chat_type_to_string(TelegramChatType t) {
    switch (t) {
        case TelegramChatType::Private:    return "private";
        case TelegramChatType::Group:      return "group";
        case TelegramChatType::Supergroup: return "supergroup";
        case TelegramChatType::Channel:    return "channel";
        case TelegramChatType::Unknown:    return "unknown";
    }
    return "unknown";
}

// ---------------------------------------------------------------------------
// Keyboard serialisation
// ---------------------------------------------------------------------------

nlohmann::json InlineKeyboardButton::to_json() const {
    nlohmann::json j;
    j["text"] = text;
    if (callback_data) j["callback_data"] = *callback_data;
    if (url) j["url"] = *url;
    if (switch_inline_query)
        j["switch_inline_query"] = *switch_inline_query;
    if (switch_inline_query_current_chat)
        j["switch_inline_query_current_chat"] = *switch_inline_query_current_chat;
    return j;
}

nlohmann::json InlineKeyboardMarkup::to_json() const {
    nlohmann::json arr = nlohmann::json::array();
    for (const auto& row : rows) {
        nlohmann::json r = nlohmann::json::array();
        for (const auto& b : row) r.push_back(b.to_json());
        arr.push_back(r);
    }
    return nlohmann::json{{"inline_keyboard", arr}};
}

nlohmann::json ReplyKeyboardButton::to_json() const {
    nlohmann::json j;
    j["text"] = text;
    if (request_contact) j["request_contact"] = true;
    if (request_location) j["request_location"] = true;
    return j;
}

nlohmann::json ReplyKeyboardMarkup::to_json() const {
    nlohmann::json arr = nlohmann::json::array();
    for (const auto& row : rows) {
        nlohmann::json r = nlohmann::json::array();
        for (const auto& b : row) r.push_back(b.to_json());
        arr.push_back(r);
    }
    nlohmann::json out = {
        {"keyboard", arr},
        {"resize_keyboard", resize_keyboard},
        {"one_time_keyboard", one_time_keyboard},
        {"selective", selective},
    };
    if (input_field_placeholder)
        out["input_field_placeholder"] = *input_field_placeholder;
    return out;
}

// ---------------------------------------------------------------------------
// Error classification
// ---------------------------------------------------------------------------

TelegramError classify_telegram_error(int http_status,
                                      const std::string& response_body) {
    TelegramError err;
    err.http_status = http_status;

    nlohmann::json body;
    try {
        body = nlohmann::json::parse(response_body);
    } catch (...) {
        // non-JSON body; ignore
    }

    if (body.is_object()) {
        if (body.contains("description") && body["description"].is_string()) {
            err.description = body["description"].get<std::string>();
        }
        if (body.contains("parameters") && body["parameters"].is_object()) {
            const auto& p = body["parameters"];
            if (p.contains("retry_after") && p["retry_after"].is_number()) {
                err.retry_after_seconds = p["retry_after"].get<double>();
            }
            if (p.contains("migrate_to_chat_id") &&
                p["migrate_to_chat_id"].is_number_integer()) {
                err.migrate_to_chat_id =
                    p["migrate_to_chat_id"].get<long long>();
            }
        }
    }

    auto desc_lower = to_lower(err.description);

    if (http_status == 429) {
        err.kind = TelegramErrorKind::FloodWait;
        if (err.retry_after_seconds <= 0) {
            std::smatch m;
            std::regex re("retry after (\\d+(?:\\.\\d+)?)");
            if (std::regex_search(err.description, m, re)) {
                err.retry_after_seconds = std::stod(m[1].str());
            } else {
                err.retry_after_seconds = 1.0;
            }
        }
        return err;
    }

    if (http_status == 401) {
        err.kind = TelegramErrorKind::Unauthorized;
        return err;
    }
    if (http_status == 403) {
        err.kind = TelegramErrorKind::Forbidden;
        return err;
    }

    if (http_status == 400) {
        if (err.migrate_to_chat_id.has_value()) {
            err.kind = TelegramErrorKind::ChatMigrated;
            return err;
        }
        if (desc_lower.find("not modified") != std::string::npos) {
            err.kind = TelegramErrorKind::NotModified;
            return err;
        }
        if (desc_lower.find("message is too long") != std::string::npos ||
            desc_lower.find("message_too_long") != std::string::npos) {
            err.kind = TelegramErrorKind::MessageTooLong;
            return err;
        }
        if (desc_lower.find("thread not found") != std::string::npos ||
            desc_lower.find("message thread not found") != std::string::npos) {
            err.kind = TelegramErrorKind::ThreadNotFound;
            return err;
        }
        if (desc_lower.find("message to be replied not found") !=
                std::string::npos ||
            desc_lower.find("replied message not found") != std::string::npos) {
            err.kind = TelegramErrorKind::ReplyNotFound;
            return err;
        }
        err.kind = TelegramErrorKind::BadRequest;
        return err;
    }

    if (http_status >= 500 && http_status < 600) {
        err.kind = TelegramErrorKind::Transient;
        return err;
    }
    if (http_status == 0) {
        err.kind = TelegramErrorKind::Transient;
        return err;
    }

    err.kind = http_status >= 200 && http_status < 300
                   ? TelegramErrorKind::None
                   : TelegramErrorKind::Fatal;
    return err;
}

// ---------------------------------------------------------------------------
// Media group buffer
// ---------------------------------------------------------------------------

bool MediaGroupBuffer::append(const nlohmann::json& message) {
    std::string id;
    if (message.contains("media_group_id") &&
        message["media_group_id"].is_string()) {
        id = message["media_group_id"].get<std::string>();
    } else {
        return true;  // singleton
    }
    auto it = buffer_.find(id);
    if (it == buffer_.end()) {
        Entry e;
        e.first_seen = std::chrono::steady_clock::now();
        e.messages.push_back(message);
        buffer_.emplace(id, std::move(e));
        return true;
    }
    it->second.messages.push_back(message);
    return false;
}

std::vector<std::vector<nlohmann::json>> MediaGroupBuffer::drain_expired(
    std::chrono::milliseconds max_age) {
    std::vector<std::vector<nlohmann::json>> out;
    auto now = std::chrono::steady_clock::now();
    for (auto it = buffer_.begin(); it != buffer_.end(); /**/) {
        if (now - it->second.first_seen >= max_age) {
            out.push_back(std::move(it->second.messages));
            it = buffer_.erase(it);
        } else {
            ++it;
        }
    }
    return out;
}

std::optional<std::vector<nlohmann::json>> MediaGroupBuffer::drain(
    const std::string& id) {
    auto it = buffer_.find(id);
    if (it == buffer_.end()) return std::nullopt;
    auto msgs = std::move(it->second.messages);
    buffer_.erase(it);
    return msgs;
}

// ---------------------------------------------------------------------------
// Message splitting — code-fence aware, honours paragraph boundaries.
// ---------------------------------------------------------------------------

std::vector<std::string> split_message_for_telegram(const std::string& text,
                                                    std::size_t max_len) {
    std::vector<std::string> out;
    if (text.empty()) return out;
    if (text.size() <= max_len) {
        out.push_back(text);
        return out;
    }

    std::string remaining = text;
    bool inside_fence = false;
    std::string fence_info;

    while (!remaining.empty()) {
        std::size_t budget = max_len;
        if (inside_fence) budget = budget > 6 ? budget - 6 : budget;

        if (remaining.size() <= budget) {
            // Recount fences in the remaining to see whether it ends still
            // inside a fence and thus needs an auto-close appended.  Start
            // outside since any re-opened fence is present at the start of
            // `remaining` as an opening token.
            bool local = false;
            std::size_t i = 0;
            while ((i = remaining.find("```", i)) != std::string::npos) {
                local = !local;
                i += 3;
            }
            if (local) remaining += "\n```";
            out.push_back(remaining);
            break;
        }

        std::size_t scan_end = std::min(budget, remaining.size());
        auto split_at = remaining.rfind("\n\n", scan_end);
        if (split_at == std::string::npos || split_at < scan_end / 2) {
            auto nl = remaining.rfind('\n', scan_end);
            if (nl != std::string::npos && nl > scan_end / 2) split_at = nl;
        }
        if (split_at == std::string::npos || split_at == 0) {
            auto sp = remaining.rfind(' ', scan_end);
            if (sp != std::string::npos && sp > 0) split_at = sp;
        }
        if (split_at == std::string::npos || split_at == 0) {
            split_at = scan_end;
        }

        std::string chunk = remaining.substr(0, split_at);
        std::size_t idx = 0;
        bool local_fence = inside_fence;
        while ((idx = chunk.find("```", idx)) != std::string::npos) {
            local_fence = !local_fence;
            if (local_fence) {
                auto nl = chunk.find('\n', idx + 3);
                fence_info = chunk.substr(
                    idx + 3,
                    nl == std::string::npos ? 0 : nl - (idx + 3));
            }
            idx += 3;
        }

        if (local_fence) chunk += "\n```";

        out.push_back(chunk);

        remaining = remaining.substr(split_at);
        while (!remaining.empty() &&
               (remaining.front() == '\n' || remaining.front() == ' ')) {
            remaining.erase(0, 1);
        }

        inside_fence = local_fence;
        if (inside_fence) {
            remaining = "```" + fence_info + "\n" + remaining;
        }
    }

    if (out.size() > 1) {
        for (std::size_t i = 0; i < out.size(); ++i) {
            out[i] += " (" + std::to_string(i + 1) + "/" +
                      std::to_string(out.size()) + ")";
        }
    }
    return out;
}

// ---------------------------------------------------------------------------
// MarkdownV2 escape / strip / high-level format
// ---------------------------------------------------------------------------

std::string TelegramAdapter::format_markdown_v2(const std::string& text) {
    static const std::string specials = R"(_*[]()~`>#+-=|{}.!\)";
    std::string result;
    result.reserve(text.size() * 2);
    for (char ch : text) {
        if (specials.find(ch) != std::string::npos) result += '\\';
        result += ch;
    }
    return result;
}

std::string strip_markdown_v2(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '\\' && i + 1 < text.size()) {
            char nxt = text[i + 1];
            static const std::string specials = R"(_*[]()~`>#+-=|{}.!\)";
            if (specials.find(nxt) != std::string::npos) {
                out += nxt;
                ++i;
                continue;
            }
        }
        out += text[i];
    }
    std::string cleaned = out;
    cleaned = std::regex_replace(cleaned, std::regex(R"(\*([^*\n]+)\*)"), "$1");
    cleaned = std::regex_replace(cleaned, std::regex(R"(_([^_\n]+)_)"), "$1");
    cleaned = std::regex_replace(cleaned, std::regex(R"(~([^~\n]+)~)"), "$1");
    cleaned = std::regex_replace(cleaned, std::regex(R"(\|\|([^|\n]+)\|\|)"), "$1");
    return cleaned;
}

std::string format_message_markdown_v2(const std::string& content) {
    if (content.empty()) return content;

    struct Placeholder {
        std::string token;
        std::string value;
    };
    std::vector<Placeholder> holders;
    int counter = 0;
    auto make_ph = [&](const std::string& value) {
        std::string token =
            std::string("\x01PH") + std::to_string(counter++) + "\x01";
        holders.push_back({token, value});
        return token;
    };

    std::string text = content;

    // 1) Fenced code blocks.
    {
        std::regex fenced_re(R"(```[\s\S]*?```)");
        std::string result;
        auto begin = std::sregex_iterator(text.begin(), text.end(), fenced_re);
        auto end = std::sregex_iterator();
        std::size_t last = 0;
        for (auto it = begin; it != end; ++it) {
            const auto& m = *it;
            result.append(text, last, m.position() - last);
            std::string raw = m.str();
            std::size_t body_start = raw.find('\n');
            std::string opening, body;
            if (body_start == std::string::npos) {
                opening = "```";
                body = raw.size() >= 6 ? raw.substr(3, raw.size() - 6) : "";
            } else {
                opening = raw.substr(0, body_start + 1);
                body = raw.substr(body_start + 1,
                                  raw.size() - (body_start + 1) - 3);
            }
            std::string esc;
            for (char c : body) {
                if (c == '\\') esc += "\\\\";
                else if (c == '`') esc += "\\`";
                else esc += c;
            }
            result.append(make_ph(opening + esc + "```"));
            last = m.position() + m.length();
        }
        result.append(text, last, std::string::npos);
        text = std::move(result);
    }

    // 2) Inline code.
    {
        std::regex inl_re(R"(`[^`\n]+`)");
        std::string result;
        auto begin = std::sregex_iterator(text.begin(), text.end(), inl_re);
        auto end = std::sregex_iterator();
        std::size_t last = 0;
        for (auto it = begin; it != end; ++it) {
            const auto& m = *it;
            result.append(text, last, m.position() - last);
            std::string s = m.str();
            std::string esc;
            for (char c : s) {
                if (c == '\\') esc += "\\\\";
                else esc += c;
            }
            result.append(make_ph(esc));
            last = m.position() + m.length();
        }
        result.append(text, last, std::string::npos);
        text = std::move(result);
    }

    auto escape_mdv2 = &TelegramAdapter::format_markdown_v2;

    // 3) Links [text](url).
    {
        std::regex link_re(R"(\[([^\]]+)\]\(([^)]+)\))");
        std::string result;
        auto begin = std::sregex_iterator(text.begin(), text.end(), link_re);
        auto end = std::sregex_iterator();
        std::size_t last = 0;
        for (auto it = begin; it != end; ++it) {
            const auto& m = *it;
            result.append(text, last, m.position() - last);
            std::string display = escape_mdv2(m[1].str());
            std::string url = m[2].str();
            std::string url_esc;
            for (char c : url) {
                if (c == '\\') url_esc += "\\\\";
                else if (c == ')') url_esc += "\\)";
                else url_esc += c;
            }
            result.append(make_ph("[" + display + "](" + url_esc + ")"));
            last = m.position() + m.length();
        }
        result.append(text, last, std::string::npos);
        text = std::move(result);
    }

    // 4) Headers ^#{1,6} text$ → *text*.
    {
        std::regex hdr_re(R"(^#{1,6}[ \t]+(.+)$)",
                          std::regex::ECMAScript | std::regex::multiline);
        std::string result;
        auto begin = std::sregex_iterator(text.begin(), text.end(), hdr_re);
        auto end = std::sregex_iterator();
        std::size_t last = 0;
        for (auto it = begin; it != end; ++it) {
            const auto& m = *it;
            result.append(text, last, m.position() - last);
            std::string inner = trim(m[1].str());
            inner = std::regex_replace(inner, std::regex(R"(\*\*(.+?)\*\*)"),
                                       "$1");
            result.append(make_ph("*" + escape_mdv2(inner) + "*"));
            last = m.position() + m.length();
        }
        result.append(text, last, std::string::npos);
        text = std::move(result);
    }

    // 5) Bold **text**.
    {
        std::regex b_re(R"(\*\*(.+?)\*\*)");
        std::string result;
        auto begin = std::sregex_iterator(text.begin(), text.end(), b_re);
        auto end = std::sregex_iterator();
        std::size_t last = 0;
        for (auto it = begin; it != end; ++it) {
            const auto& m = *it;
            result.append(text, last, m.position() - last);
            result.append(make_ph("*" + escape_mdv2(m[1].str()) + "*"));
            last = m.position() + m.length();
        }
        result.append(text, last, std::string::npos);
        text = std::move(result);
    }

    // 6) Italic *text*.
    {
        std::regex i_re(R"(\*([^*\n]+)\*)");
        std::string result;
        auto begin = std::sregex_iterator(text.begin(), text.end(), i_re);
        auto end = std::sregex_iterator();
        std::size_t last = 0;
        for (auto it = begin; it != end; ++it) {
            const auto& m = *it;
            result.append(text, last, m.position() - last);
            result.append(make_ph("_" + escape_mdv2(m[1].str()) + "_"));
            last = m.position() + m.length();
        }
        result.append(text, last, std::string::npos);
        text = std::move(result);
    }

    // 7) Strike ~~text~~.
    {
        std::regex s_re(R"(~~(.+?)~~)");
        std::string result;
        auto begin = std::sregex_iterator(text.begin(), text.end(), s_re);
        auto end = std::sregex_iterator();
        std::size_t last = 0;
        for (auto it = begin; it != end; ++it) {
            const auto& m = *it;
            result.append(text, last, m.position() - last);
            result.append(make_ph("~" + escape_mdv2(m[1].str()) + "~"));
            last = m.position() + m.length();
        }
        result.append(text, last, std::string::npos);
        text = std::move(result);
    }

    // 8) Spoiler ||text||.
    {
        std::regex sp_re(R"(\|\|(.+?)\|\|)");
        std::string result;
        auto begin = std::sregex_iterator(text.begin(), text.end(), sp_re);
        auto end = std::sregex_iterator();
        std::size_t last = 0;
        for (auto it = begin; it != end; ++it) {
            const auto& m = *it;
            result.append(text, last, m.position() - last);
            result.append(make_ph("||" + escape_mdv2(m[1].str()) + "||"));
            last = m.position() + m.length();
        }
        result.append(text, last, std::string::npos);
        text = std::move(result);
    }

    // 9) Escape remaining specials.
    text = TelegramAdapter::format_markdown_v2(text);

    // 10) Restore placeholders in reverse-insertion order.
    for (auto it = holders.rbegin(); it != holders.rend(); ++it) {
        std::size_t pos = 0;
        while ((pos = text.find(it->token, pos)) != std::string::npos) {
            text.replace(pos, it->token.size(), it->value);
            pos += it->value.size();
        }
    }

    return text;
}

// ---------------------------------------------------------------------------
// Static parsing helpers
// ---------------------------------------------------------------------------

std::optional<long long> TelegramAdapter::parse_forum_topic(
    const nlohmann::json& message) {
    if (message.is_object() && message.contains("message_thread_id") &&
        message["message_thread_id"].is_number_integer()) {
        return message["message_thread_id"].get<long long>();
    }
    return std::nullopt;
}

std::optional<std::string> TelegramAdapter::parse_media_group_id(
    const nlohmann::json& message) {
    if (message.is_object() && message.contains("media_group_id") &&
        message["media_group_id"].is_string()) {
        return message["media_group_id"].get<std::string>();
    }
    return std::nullopt;
}

TelegramChatType TelegramAdapter::parse_message_chat_type(
    const nlohmann::json& message) {
    if (!message.is_object()) return TelegramChatType::Unknown;
    if (!message.contains("chat") || !message["chat"].is_object())
        return TelegramChatType::Unknown;
    const auto& chat = message["chat"];
    if (!chat.contains("type") || !chat["type"].is_string())
        return TelegramChatType::Unknown;
    return parse_chat_type(chat["type"].get<std::string>());
}

bool TelegramAdapter::is_forum_topic_created(const nlohmann::json& message) {
    return message.is_object() && message.contains("forum_topic_created") &&
           message["forum_topic_created"].is_object();
}

bool TelegramAdapter::is_forum_topic_closed(const nlohmann::json& message) {
    return message.is_object() && message.contains("forum_topic_closed");
}

std::optional<long long> TelegramAdapter::parse_reply_to_message_id(
    const nlohmann::json& message) {
    if (!message.is_object()) return std::nullopt;
    if (!message.contains("reply_to_message") ||
        !message["reply_to_message"].is_object())
        return std::nullopt;
    const auto& rtm = message["reply_to_message"];
    if (rtm.contains("message_id") && rtm["message_id"].is_number_integer()) {
        return rtm["message_id"].get<long long>();
    }
    return std::nullopt;
}

std::optional<long long> TelegramAdapter::parse_migrate_to_chat_id(
    const nlohmann::json& message) {
    if (!message.is_object()) return std::nullopt;
    if (message.contains("migrate_to_chat_id") &&
        message["migrate_to_chat_id"].is_number_integer()) {
        return message["migrate_to_chat_id"].get<long long>();
    }
    return std::nullopt;
}

std::string TelegramAdapter::parse_update_kind(const nlohmann::json& update) {
    if (!update.is_object()) return {};
    static const std::vector<std::string> kinds = {
        "message",             "edited_message",       "channel_post",
        "edited_channel_post", "callback_query",       "inline_query",
        "chosen_inline_result", "shipping_query",      "pre_checkout_query",
        "poll",                "poll_answer",          "my_chat_member",
        "chat_member",         "chat_join_request"};
    for (const auto& k : kinds) {
        if (update.contains(k)) return k;
    }
    return {};
}

bool TelegramAdapter::message_mentions_bot(const nlohmann::json& message,
                                           const std::string& bot_username,
                                           std::optional<long long> bot_id) {
    if (!message.is_object()) return false;
    auto check_source = [&](const std::string& text,
                            const nlohmann::json& entities) -> bool {
        if (!bot_username.empty()) {
            std::string needle = "@" + bot_username;
            if (contains_ci(text, needle)) return true;
        }
        if (entities.is_array()) {
            for (const auto& e : entities) {
                std::string etype = e.value("type", "");
                auto et = to_lower(etype);
                if (et == "mention" && !bot_username.empty()) {
                    int offset = e.value("offset", -1);
                    int length = e.value("length", 0);
                    if (offset >= 0 && length > 0 &&
                        static_cast<std::size_t>(offset + length) <=
                            text.size()) {
                        std::string slice = text.substr(offset, length);
                        if (to_lower(trim(slice)) ==
                            to_lower("@" + bot_username))
                            return true;
                    }
                }
                if (et == "text_mention" && bot_id.has_value() &&
                    e.contains("user") && e["user"].is_object() &&
                    e["user"].value("id", 0LL) == *bot_id) {
                    return true;
                }
            }
        }
        return false;
    };

    std::string text = message.value("text", "");
    nlohmann::json entities =
        message.value("entities", nlohmann::json::array());
    if (check_source(text, entities)) return true;

    std::string caption = message.value("caption", "");
    nlohmann::json cap_entities =
        message.value("caption_entities", nlohmann::json::array());
    if (check_source(caption, cap_entities)) return true;

    return false;
}

std::string TelegramAdapter::clean_bot_trigger_text(
    const std::string& text, const std::string& bot_username) {
    if (text.empty() || bot_username.empty()) return text;
    std::string escaped;
    for (char c : bot_username) {
        if (std::isalnum(static_cast<unsigned char>(c)) || c == '_') {
            escaped += c;
        } else {
            escaped += '\\';
            escaped += c;
        }
    }
    try {
        std::regex re("@" + escaped + R"(\b[,:\-]*\s*)", std::regex::icase);
        std::string out = std::regex_replace(text, re, "");
        out = trim(out);
        return out.empty() ? text : out;
    } catch (...) {
        return text;
    }
}

std::string TelegramAdapter::classify_callback_data(const std::string& data) {
    if (data.rfind("ea:", 0) == 0) return "ea";
    if (data.rfind("mp:", 0) == 0) return "mp";
    if (data.rfind("mm:", 0) == 0) return "mm";
    if (data.rfind("mg:", 0) == 0) return "mg";
    if (data == "mb") return "mb";
    if (data.rfind("mx", 0) == 0) return "mx";
    if (data.rfind("update_prompt:", 0) == 0) return "update_prompt";
    return {};
}

std::string TelegramAdapter::render_location_prompt(
    const nlohmann::json& message) {
    double lat = 0.0, lon = 0.0;
    std::string title, address;
    bool have_location = false;
    if (message.contains("venue") && message["venue"].is_object()) {
        const auto& v = message["venue"];
        title = v.value("title", "");
        address = v.value("address", "");
        if (v.contains("location") && v["location"].is_object()) {
            lat = v["location"].value("latitude", 0.0);
            lon = v["location"].value("longitude", 0.0);
            have_location = true;
        }
    } else if (message.contains("location") && message["location"].is_object()) {
        lat = message["location"].value("latitude", 0.0);
        lon = message["location"].value("longitude", 0.0);
        have_location = true;
    }
    if (!have_location) return {};

    std::ostringstream oss;
    oss << "[The user shared a location pin.]\n";
    if (!title.empty()) oss << "Venue: " << title << "\n";
    if (!address.empty()) oss << "Address: " << address << "\n";
    oss << "latitude: " << lat << "\n";
    oss << "longitude: " << lon << "\n";
    oss << "Map: https://www.google.com/maps/search/?api=1&query=" << lat << ","
        << lon << "\n";
    oss << "Ask what they'd like to find nearby (restaurants, cafes, etc.) "
           "and any preferences.";
    return oss.str();
}

// ---------------------------------------------------------------------------
// Policy
// ---------------------------------------------------------------------------

bool TelegramAdapter::should_process_message(const nlohmann::json& message,
                                             bool is_command) const {
    auto chat_type = parse_message_chat_type(message);
    if (chat_type != TelegramChatType::Group &&
        chat_type != TelegramChatType::Supergroup) {
        return true;
    }
    if (message.contains("chat") && message["chat"].is_object() &&
        message["chat"].contains("id")) {
        std::string cid;
        if (message["chat"]["id"].is_number_integer())
            cid = std::to_string(message["chat"]["id"].get<long long>());
        else if (message["chat"]["id"].is_string())
            cid = message["chat"]["id"].get<std::string>();
        if (cfg_.free_response_chats.count(cid)) return true;
    }
    if (!cfg_.require_mention) return true;
    if (is_command) return true;

    if (message.contains("reply_to_message") &&
        message["reply_to_message"].is_object()) {
        const auto& rtm = message["reply_to_message"];
        if (rtm.contains("from") && rtm["from"].is_object()) {
            auto fid = rtm["from"].value("id", 0LL);
            if (fid == bot_id_ && bot_id_ != 0) return true;
        }
    }

    if (message_mentions_bot(
            message, bot_username_,
            bot_id_ ? std::optional<long long>(bot_id_) : std::nullopt))
        return true;

    for (const auto& pat : cfg_.mention_patterns) {
        try {
            std::regex re(pat, std::regex::icase);
            std::string text = message.value("text", "");
            if (!text.empty() && std::regex_search(text, re)) return true;
            std::string caption = message.value("caption", "");
            if (!caption.empty() && std::regex_search(caption, re)) return true;
        } catch (...) {
            continue;
        }
    }
    return false;
}

// ---------------------------------------------------------------------------
// Construction / transport
// ---------------------------------------------------------------------------

TelegramAdapter::TelegramAdapter(Config cfg) : cfg_(std::move(cfg)) {}

TelegramAdapter::TelegramAdapter(Config cfg,
                                 hermes::llm::HttpTransport* transport)
    : cfg_(std::move(cfg)), transport_(transport) {}

TelegramAdapter::~TelegramAdapter() {
    if (connected_ && !cfg_.bot_token.empty()) {
        try {
            hermes::gateway::release_scoped_lock(
                hermes::gateway::platform_to_string(platform()),
                cfg_.bot_token);
        } catch (...) {
        }
    }
}

hermes::llm::HttpTransport* TelegramAdapter::get_transport() {
    if (transport_) return transport_;
    return hermes::llm::get_default_transport();
}

std::string TelegramAdapter::api_url(const std::string& method) const {
    std::string base = cfg_.base_url.empty()
                           ? std::string("https://api.telegram.org")
                           : cfg_.base_url;
    while (!base.empty() && base.back() == '/') base.pop_back();
    return base + "/bot" + cfg_.bot_token + "/" + method;
}

std::string TelegramAdapter::file_api_url(const std::string& file_path) const {
    std::string base = cfg_.base_file_url.empty()
                           ? std::string("https://api.telegram.org")
                           : cfg_.base_file_url;
    while (!base.empty() && base.back() == '/') base.pop_back();
    return base + "/file/bot" + cfg_.bot_token + "/" + file_path;
}

// ---------------------------------------------------------------------------
// Low-level API dispatch
// ---------------------------------------------------------------------------

std::optional<nlohmann::json> TelegramAdapter::call_api(
    const std::string& method, const nlohmann::json& payload,
    TelegramError* out_error) {
    auto* transport = get_transport();
    if (!transport) {
        if (out_error) {
            out_error->kind = TelegramErrorKind::Fatal;
            out_error->description = "no transport";
        }
        return std::nullopt;
    }
    if (cfg_.bot_token.empty()) {
        if (out_error) {
            out_error->kind = TelegramErrorKind::Unauthorized;
            out_error->description = "empty bot token";
        }
        return std::nullopt;
    }

    std::string url = api_url(method);
    std::string body = payload.dump();

    hermes::llm::HttpTransport::Response resp;
    try {
        resp = transport->post_json(
            url, {{"Content-Type", "application/json"}}, body);
    } catch (const std::exception& e) {
        if (out_error) {
            out_error->kind = TelegramErrorKind::Transient;
            out_error->description = e.what();
        }
        return std::nullopt;
    }

    if (resp.status_code == 200) {
        try {
            auto parsed = nlohmann::json::parse(resp.body);
            if (parsed.is_object() && parsed.value("ok", false)) {
                if (parsed.contains("result")) return parsed["result"];
                return nlohmann::json::object();
            }
            auto err = classify_telegram_error(400, resp.body);
            if (out_error) *out_error = err;
            return std::nullopt;
        } catch (const std::exception& e) {
            if (out_error) {
                out_error->kind = TelegramErrorKind::Transient;
                out_error->description =
                    std::string("parse error: ") + e.what();
            }
            return std::nullopt;
        }
    }

    auto err = classify_telegram_error(resp.status_code, resp.body);
    if (err.kind == TelegramErrorKind::FloodWait) {
        auto dur = std::chrono::milliseconds(
            static_cast<long long>(err.retry_after_seconds * 1000));
        flood_wait_until_ = std::chrono::steady_clock::now() + dur;
    }
    if (err.kind == TelegramErrorKind::ChatMigrated &&
        err.migrate_to_chat_id.has_value() && payload.contains("chat_id")) {
        std::string old_id;
        if (payload["chat_id"].is_number_integer())
            old_id = std::to_string(payload["chat_id"].get<long long>());
        else if (payload["chat_id"].is_string())
            old_id = payload["chat_id"].get<std::string>();
        if (!old_id.empty()) {
            std::lock_guard<std::mutex> lk(migrated_mu_);
            migrated_chats_[old_id] = std::to_string(*err.migrate_to_chat_id);
        }
    }
    if (out_error) *out_error = err;
    return std::nullopt;
}

// ---------------------------------------------------------------------------
// BasePlatformAdapter overrides
// ---------------------------------------------------------------------------

bool TelegramAdapter::connect() {
    if (cfg_.bot_token.empty()) return false;
    if (!hermes::gateway::acquire_scoped_lock(
            hermes::gateway::platform_to_string(platform()), cfg_.bot_token,
            {})) {
        return false;
    }

    auto* transport = get_transport();
    if (!transport) {
        hermes::gateway::release_scoped_lock(
            hermes::gateway::platform_to_string(platform()), cfg_.bot_token);
        return false;
    }

    TelegramError err;
    auto result = call_api("getMe", nlohmann::json::object(), &err);
    if (!result) {
        hermes::gateway::release_scoped_lock(
            hermes::gateway::platform_to_string(platform()), cfg_.bot_token);
        return false;
    }
    if (result->contains("username") && (*result)["username"].is_string()) {
        bot_username_ = (*result)["username"].get<std::string>();
    }
    if (result->contains("id") && (*result)["id"].is_number_integer()) {
        bot_id_ = (*result)["id"].get<long long>();
    }
    connected_ = true;
    return true;
}

void TelegramAdapter::disconnect() {
    if (connected_ || !cfg_.bot_token.empty()) {
        hermes::gateway::release_scoped_lock(
            hermes::gateway::platform_to_string(platform()), cfg_.bot_token);
    }
    connected_ = false;
}

bool TelegramAdapter::send(const std::string& chat_id,
                           const std::string& content) {
    TelegramSendOptions opts;
    auto res = send_message(chat_id, content, opts);
    return res.ok;
}

void TelegramAdapter::send_typing(const std::string& chat_id) {
    nlohmann::json payload = {{"chat_id", chat_id}, {"action", "typing"}};
    call_api("sendChatAction", payload, nullptr);
}

// ---------------------------------------------------------------------------
// send_message / send_chunk
// ---------------------------------------------------------------------------

TelegramAdapter::ChunkResult TelegramAdapter::send_chunk(
    const std::string& chat_id, const std::string& chunk,
    const SendOptions& opts, std::optional<long long> reply_to_override,
    std::optional<long long> thread_override) {
    ChunkResult out;
    nlohmann::json payload = {
        {"chat_id", chat_id},
        {"text", chunk},
    };
    if (!opts.parse_mode.empty()) payload["parse_mode"] = opts.parse_mode;
    if (opts.disable_web_page_preview)
        payload["disable_web_page_preview"] = true;
    if (opts.disable_notification) payload["disable_notification"] = true;
    if (opts.protect_content) payload["protect_content"] = true;
    if (reply_to_override) payload["reply_to_message_id"] = *reply_to_override;
    if (thread_override) payload["message_thread_id"] = *thread_override;
    if (opts.inline_keyboard)
        payload["reply_markup"] = opts.inline_keyboard->to_json();
    else if (opts.reply_keyboard)
        payload["reply_markup"] = opts.reply_keyboard->to_json();
    else if (opts.remove_reply_keyboard)
        payload["reply_markup"] = {{"remove_keyboard", true}};

    int max_retry = std::max(1, cfg_.max_send_retries);
    for (int attempt = 0; attempt < max_retry; ++attempt) {
        TelegramError err;
        auto res = call_api("sendMessage", payload, &err);
        if (res) {
            out.ok = true;
            if (res->contains("message_id") &&
                (*res)["message_id"].is_number_integer()) {
                out.message_id = (*res)["message_id"].get<long long>();
            }
            return out;
        }

        out.error = err;
        if (err.kind == TelegramErrorKind::ThreadNotFound &&
            payload.contains("message_thread_id")) {
            payload.erase("message_thread_id");
            continue;
        }
        if (err.kind == TelegramErrorKind::ReplyNotFound &&
            payload.contains("reply_to_message_id")) {
            payload.erase("reply_to_message_id");
            continue;
        }
        if (err.kind == TelegramErrorKind::FloodWait) {
            if (err.retry_after_seconds > cfg_.max_flood_wait_s) {
                return out;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(
                static_cast<long long>(err.retry_after_seconds * 1000)));
            continue;
        }
        if (err.kind == TelegramErrorKind::BadRequest &&
            payload.value("parse_mode", "") != "") {
            auto desc = to_lower(err.description);
            if (desc.find("parse") != std::string::npos ||
                desc.find("markdown") != std::string::npos ||
                desc.find("entity") != std::string::npos) {
                payload["text"] = strip_markdown_v2(chunk);
                payload.erase("parse_mode");
                continue;
            }
            return out;
        }
        if (err.kind == TelegramErrorKind::ChatMigrated &&
            err.migrate_to_chat_id.has_value()) {
            payload["chat_id"] = std::to_string(*err.migrate_to_chat_id);
            continue;
        }
        if (err.kind == TelegramErrorKind::Transient) {
            int delay_ms = 200 * (1 << attempt);
            std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
            continue;
        }
        return out;
    }
    return out;
}

TelegramAdapter::SendResult TelegramAdapter::send_message(
    const std::string& chat_id_raw, const std::string& content,
    const SendOptions& opts) {
    SendResult out;
    if (cfg_.bot_token.empty()) {
        out.last_error.kind = TelegramErrorKind::Unauthorized;
        out.last_error.description = "no bot token";
        return out;
    }

    std::string chat_id;
    {
        std::lock_guard<std::mutex> lk(migrated_mu_);
        auto it = migrated_chats_.find(chat_id_raw);
        chat_id = it != migrated_chats_.end() ? it->second : chat_id_raw;
    }

    if (trim(content).empty()) {
        out.ok = true;
        return out;
    }

    std::string formatted =
        opts.parse_mode == "MarkdownV2" ? format_message(content) : content;
    auto chunks = split_message_for_telegram(formatted, kMaxMessageLength);
    if (chunks.empty()) {
        out.ok = true;
        return out;
    }

    for (std::size_t i = 0; i < chunks.size(); ++i) {
        const auto& chunk = chunks[i];

        bool should_reply = false;
        if (opts.reply_to_message_id.has_value()) {
            if (cfg_.reply_to_mode == "all") should_reply = true;
            else if (cfg_.reply_to_mode == "first") should_reply = i == 0;
            else if (cfg_.reply_to_mode == "off") should_reply = false;
            else should_reply = i == 0;
        }

        auto reply_override = should_reply ? opts.reply_to_message_id
                                           : std::optional<long long>{};
        auto thread_override = opts.message_thread_id;

        auto res = send_chunk(chat_id, chunk, opts, reply_override,
                              thread_override);
        if (!res.ok) {
            out.last_error = res.error;
            if (out.message_ids.empty()) {
                out.ok = false;
                return out;
            }
            out.ok = true;
            return out;
        }
        out.message_ids.push_back(res.message_id);
    }
    out.ok = true;
    return out;
}

// ---------------------------------------------------------------------------
// edit / delete / forward / copy / pin
// ---------------------------------------------------------------------------

TelegramAdapter::SendResult TelegramAdapter::edit_message_text(
    const std::string& chat_id, long long message_id,
    const std::string& content, const std::string& parse_mode) {
    SendResult out;
    std::string formatted =
        parse_mode == "MarkdownV2" ? format_message(content) : content;
    nlohmann::json payload = {
        {"chat_id", chat_id},
        {"message_id", message_id},
        {"text", formatted},
    };
    if (!parse_mode.empty()) payload["parse_mode"] = parse_mode;

    TelegramError err;
    auto res = call_api("editMessageText", payload, &err);
    if (res) {
        out.ok = true;
        out.message_ids.push_back(message_id);
        return out;
    }
    if (err.kind == TelegramErrorKind::NotModified) {
        out.ok = true;
        out.message_ids.push_back(message_id);
        return out;
    }
    if (err.kind == TelegramErrorKind::MessageTooLong) {
        std::string truncated = content.substr(0, kMaxMessageLength - 4) + "…";
        payload["text"] = truncated;
        payload.erase("parse_mode");
        TelegramError err2;
        auto r2 = call_api("editMessageText", payload, &err2);
        if (r2) {
            out.ok = true;
            out.message_ids.push_back(message_id);
            return out;
        }
        out.last_error = err2;
        return out;
    }
    if (err.kind == TelegramErrorKind::BadRequest) {
        payload["text"] = content;
        payload.erase("parse_mode");
        TelegramError err2;
        auto r2 = call_api("editMessageText", payload, &err2);
        if (r2) {
            out.ok = true;
            out.message_ids.push_back(message_id);
            return out;
        }
        out.last_error = err2;
        return out;
    }
    if (err.kind == TelegramErrorKind::FloodWait &&
        err.retry_after_seconds <= cfg_.max_flood_wait_s) {
        std::this_thread::sleep_for(std::chrono::milliseconds(
            static_cast<long long>(err.retry_after_seconds * 1000)));
        TelegramError err2;
        auto r2 = call_api("editMessageText", payload, &err2);
        if (r2) {
            out.ok = true;
            out.message_ids.push_back(message_id);
            return out;
        }
        out.last_error = err2;
        return out;
    }
    out.last_error = err;
    return out;
}

bool TelegramAdapter::delete_message(const std::string& chat_id,
                                     long long message_id) {
    nlohmann::json payload = {{"chat_id", chat_id},
                              {"message_id", message_id}};
    return call_api("deleteMessage", payload, nullptr).has_value();
}

TelegramAdapter::SendResult TelegramAdapter::forward_message(
    const std::string& from_chat_id, const std::string& to_chat_id,
    long long message_id) {
    SendResult out;
    nlohmann::json payload = {
        {"chat_id", to_chat_id},
        {"from_chat_id", from_chat_id},
        {"message_id", message_id},
    };
    TelegramError err;
    auto res = call_api("forwardMessage", payload, &err);
    if (!res) { out.last_error = err; return out; }
    out.ok = true;
    if (res->contains("message_id") && (*res)["message_id"].is_number_integer())
        out.message_ids.push_back((*res)["message_id"].get<long long>());
    return out;
}

TelegramAdapter::SendResult TelegramAdapter::copy_message(
    const std::string& from_chat_id, const std::string& to_chat_id,
    long long message_id, const std::optional<std::string>& caption) {
    SendResult out;
    nlohmann::json payload = {
        {"chat_id", to_chat_id},
        {"from_chat_id", from_chat_id},
        {"message_id", message_id},
    };
    if (caption) payload["caption"] = cap_caption(*caption);
    TelegramError err;
    auto res = call_api("copyMessage", payload, &err);
    if (!res) { out.last_error = err; return out; }
    out.ok = true;
    if (res->contains("message_id") && (*res)["message_id"].is_number_integer())
        out.message_ids.push_back((*res)["message_id"].get<long long>());
    return out;
}

bool TelegramAdapter::pin_chat_message(const std::string& chat_id,
                                       long long message_id,
                                       bool disable_notification) {
    nlohmann::json payload = {
        {"chat_id", chat_id},
        {"message_id", message_id},
        {"disable_notification", disable_notification},
    };
    return call_api("pinChatMessage", payload, nullptr).has_value();
}

bool TelegramAdapter::unpin_chat_message(
    const std::string& chat_id, std::optional<long long> message_id) {
    nlohmann::json payload = {{"chat_id", chat_id}};
    if (message_id) payload["message_id"] = *message_id;
    return call_api("unpinChatMessage", payload, nullptr).has_value();
}

// ---------------------------------------------------------------------------
// reactions / commands / menu / chat info
// ---------------------------------------------------------------------------

bool TelegramAdapter::set_reaction(const std::string& chat_id,
                                   long long message_id,
                                   const std::string& emoji) {
    nlohmann::json payload = {
        {"chat_id", chat_id},
        {"message_id", message_id},
        {"reaction",
         nlohmann::json::array(
             {nlohmann::json{{"type", "emoji"}, {"emoji", emoji}}})},
    };
    return call_api("setMessageReaction", payload, nullptr).has_value();
}

bool TelegramAdapter::clear_reactions(const std::string& chat_id,
                                      long long message_id) {
    nlohmann::json payload = {
        {"chat_id", chat_id},
        {"message_id", message_id},
        {"reaction", nlohmann::json::array()},
    };
    return call_api("setMessageReaction", payload, nullptr).has_value();
}

bool TelegramAdapter::set_my_commands(
    const std::vector<std::pair<std::string, std::string>>& commands) {
    if (cfg_.bot_token.empty()) return false;
    nlohmann::json arr = nlohmann::json::array();
    for (const auto& [name, desc] : commands) {
        if (name.empty()) continue;
        std::string d = desc;
        if (d.size() > 256) d.resize(256);
        if (d.empty()) d = name;
        arr.push_back({{"command", name}, {"description", d}});
    }
    return call_api("setMyCommands", {{"commands", arr}}, nullptr).has_value();
}

bool TelegramAdapter::delete_my_commands() {
    return call_api("deleteMyCommands", nlohmann::json::object(), nullptr)
        .has_value();
}

bool TelegramAdapter::set_chat_menu_button(
    const std::string& chat_id, const std::string& button_type,
    const std::optional<std::string>& text,
    const std::optional<std::string>& web_app_url) {
    nlohmann::json button = {{"type", button_type}};
    if (text) button["text"] = *text;
    if (web_app_url) button["web_app"] = {{"url", *web_app_url}};
    nlohmann::json payload = {{"chat_id", chat_id}, {"menu_button", button}};
    return call_api("setChatMenuButton", payload, nullptr).has_value();
}

nlohmann::json TelegramAdapter::get_me() {
    auto res = call_api("getMe", nlohmann::json::object(), nullptr);
    return res.value_or(nlohmann::json::object());
}

nlohmann::json TelegramAdapter::get_chat(const std::string& chat_id) {
    auto res = call_api("getChat", {{"chat_id", chat_id}}, nullptr);
    return res.value_or(nlohmann::json::object());
}

nlohmann::json TelegramAdapter::get_chat_member(const std::string& chat_id,
                                                const std::string& user_id) {
    auto res = call_api("getChatMember",
                        {{"chat_id", chat_id}, {"user_id", user_id}}, nullptr);
    return res.value_or(nlohmann::json::object());
}

nlohmann::json TelegramAdapter::get_user_profile_photos(
    const std::string& user_id, int limit) {
    nlohmann::json payload = {{"user_id", user_id}, {"limit", limit}};
    auto res = call_api("getUserProfilePhotos", payload, nullptr);
    return res.value_or(nlohmann::json::object());
}

// ---------------------------------------------------------------------------
// File API
// ---------------------------------------------------------------------------

std::optional<std::string> TelegramAdapter::get_file_download_url(
    const std::string& file_id) {
    auto res = call_api("getFile", {{"file_id", file_id}}, nullptr);
    if (!res) return std::nullopt;
    if (!res->contains("file_path") || !(*res)["file_path"].is_string())
        return std::nullopt;
    return file_api_url((*res)["file_path"].get<std::string>());
}

std::string TelegramAdapter::download_file(const std::string& file_id) {
    auto url = get_file_download_url(file_id);
    if (!url) return {};
    auto* transport = get_transport();
    if (!transport) return {};
    try {
        auto resp = transport->get(*url, {});
        if (resp.status_code == 200) return resp.body;
    } catch (...) {
    }
    return {};
}

// ---------------------------------------------------------------------------
// Media send helpers
// ---------------------------------------------------------------------------

namespace {
nlohmann::json build_media_payload(const std::string& chat_id,
                                   const std::string& key,
                                   const std::string& value,
                                   const std::optional<std::string>& caption,
                                   const TelegramAdapter::SendOptions& opts) {
    nlohmann::json payload = {{"chat_id", chat_id}, {key, value}};
    if (caption) payload["caption"] = cap_caption(*caption);
    if (opts.reply_to_message_id)
        payload["reply_to_message_id"] = *opts.reply_to_message_id;
    if (opts.message_thread_id)
        payload["message_thread_id"] = *opts.message_thread_id;
    if (opts.disable_notification) payload["disable_notification"] = true;
    if (opts.protect_content) payload["protect_content"] = true;
    if (opts.inline_keyboard)
        payload["reply_markup"] = opts.inline_keyboard->to_json();
    return payload;
}
}  // namespace

TelegramAdapter::SendResult TelegramAdapter::send_photo(
    const std::string& chat_id, const std::string& photo,
    const std::optional<std::string>& caption, const SendOptions& opts) {
    SendResult out;
    auto payload = build_media_payload(chat_id, "photo", photo, caption, opts);
    TelegramError err;
    auto res = call_api("sendPhoto", payload, &err);
    if (!res) { out.last_error = err; return out; }
    out.ok = true;
    if (res->contains("message_id"))
        out.message_ids.push_back((*res)["message_id"].get<long long>());
    return out;
}

TelegramAdapter::SendResult TelegramAdapter::send_document(
    const std::string& chat_id, const std::string& document,
    const std::optional<std::string>& filename,
    const std::optional<std::string>& caption, const SendOptions& opts) {
    SendResult out;
    auto payload =
        build_media_payload(chat_id, "document", document, caption, opts);
    if (filename) payload["filename"] = *filename;
    TelegramError err;
    auto res = call_api("sendDocument", payload, &err);
    if (!res) { out.last_error = err; return out; }
    out.ok = true;
    if (res->contains("message_id"))
        out.message_ids.push_back((*res)["message_id"].get<long long>());
    return out;
}

TelegramAdapter::SendResult TelegramAdapter::send_video(
    const std::string& chat_id, const std::string& video,
    const std::optional<std::string>& caption, const SendOptions& opts) {
    SendResult out;
    auto payload = build_media_payload(chat_id, "video", video, caption, opts);
    TelegramError err;
    auto res = call_api("sendVideo", payload, &err);
    if (!res) { out.last_error = err; return out; }
    out.ok = true;
    if (res->contains("message_id"))
        out.message_ids.push_back((*res)["message_id"].get<long long>());
    return out;
}

TelegramAdapter::SendResult TelegramAdapter::send_audio(
    const std::string& chat_id, const std::string& audio,
    const std::optional<std::string>& caption, const SendOptions& opts) {
    SendResult out;
    auto payload = build_media_payload(chat_id, "audio", audio, caption, opts);
    TelegramError err;
    auto res = call_api("sendAudio", payload, &err);
    if (!res) { out.last_error = err; return out; }
    out.ok = true;
    if (res->contains("message_id"))
        out.message_ids.push_back((*res)["message_id"].get<long long>());
    return out;
}

TelegramAdapter::SendResult TelegramAdapter::send_voice(
    const std::string& chat_id, const std::string& voice,
    const std::optional<std::string>& caption, const SendOptions& opts) {
    SendResult out;
    auto payload = build_media_payload(chat_id, "voice", voice, caption, opts);
    TelegramError err;
    auto res = call_api("sendVoice", payload, &err);
    if (!res) { out.last_error = err; return out; }
    out.ok = true;
    if (res->contains("message_id"))
        out.message_ids.push_back((*res)["message_id"].get<long long>());
    return out;
}

TelegramAdapter::SendResult TelegramAdapter::send_animation(
    const std::string& chat_id, const std::string& animation,
    const std::optional<std::string>& caption, const SendOptions& opts) {
    SendResult out;
    auto payload =
        build_media_payload(chat_id, "animation", animation, caption, opts);
    TelegramError err;
    auto res = call_api("sendAnimation", payload, &err);
    if (!res) { out.last_error = err; return out; }
    out.ok = true;
    if (res->contains("message_id"))
        out.message_ids.push_back((*res)["message_id"].get<long long>());
    return out;
}

TelegramAdapter::SendResult TelegramAdapter::send_sticker(
    const std::string& chat_id, const std::string& sticker,
    const SendOptions& opts) {
    SendResult out;
    auto payload =
        build_media_payload(chat_id, "sticker", sticker, std::nullopt, opts);
    TelegramError err;
    auto res = call_api("sendSticker", payload, &err);
    if (!res) { out.last_error = err; return out; }
    out.ok = true;
    if (res->contains("message_id"))
        out.message_ids.push_back((*res)["message_id"].get<long long>());
    return out;
}

TelegramAdapter::SendResult TelegramAdapter::send_poll(
    const std::string& chat_id, const std::string& question,
    const std::vector<std::string>& options, bool is_anonymous, bool is_quiz,
    std::optional<int> correct_option_id) {
    SendResult out;
    nlohmann::json opts_arr = nlohmann::json::array();
    for (const auto& o : options) opts_arr.push_back(o);
    nlohmann::json payload = {
        {"chat_id", chat_id},
        {"question", question},
        {"options", opts_arr},
        {"is_anonymous", is_anonymous},
        {"type", is_quiz ? "quiz" : "regular"},
    };
    if (is_quiz && correct_option_id)
        payload["correct_option_id"] = *correct_option_id;
    TelegramError err;
    auto res = call_api("sendPoll", payload, &err);
    if (!res) { out.last_error = err; return out; }
    out.ok = true;
    if (res->contains("message_id"))
        out.message_ids.push_back((*res)["message_id"].get<long long>());
    return out;
}

TelegramAdapter::SendResult TelegramAdapter::send_location(
    const std::string& chat_id, double latitude, double longitude,
    const SendOptions& opts) {
    SendResult out;
    nlohmann::json payload = {
        {"chat_id", chat_id},
        {"latitude", latitude},
        {"longitude", longitude},
    };
    if (opts.reply_to_message_id)
        payload["reply_to_message_id"] = *opts.reply_to_message_id;
    if (opts.message_thread_id)
        payload["message_thread_id"] = *opts.message_thread_id;
    TelegramError err;
    auto res = call_api("sendLocation", payload, &err);
    if (!res) { out.last_error = err; return out; }
    out.ok = true;
    if (res->contains("message_id"))
        out.message_ids.push_back((*res)["message_id"].get<long long>());
    return out;
}

// ---------------------------------------------------------------------------
// Callback / webhook / polling
// ---------------------------------------------------------------------------

bool TelegramAdapter::answer_callback_query(
    const std::string& callback_query_id,
    const std::optional<std::string>& text, bool show_alert,
    std::optional<int> cache_time) {
    nlohmann::json payload = {{"callback_query_id", callback_query_id}};
    if (text) payload["text"] = *text;
    if (show_alert) payload["show_alert"] = true;
    if (cache_time) payload["cache_time"] = *cache_time;
    return call_api("answerCallbackQuery", payload, nullptr).has_value();
}

bool TelegramAdapter::set_webhook(
    const std::string& url, const std::optional<std::string>& secret_token,
    const std::vector<std::string>& allowed_updates) {
    nlohmann::json payload = {{"url", url}};
    if (secret_token) payload["secret_token"] = *secret_token;
    if (!allowed_updates.empty()) {
        nlohmann::json arr = nlohmann::json::array();
        for (const auto& u : allowed_updates) arr.push_back(u);
        payload["allowed_updates"] = arr;
    }
    return call_api("setWebhook", payload, nullptr).has_value();
}

bool TelegramAdapter::delete_webhook(bool drop_pending_updates) {
    nlohmann::json payload = {{"drop_pending_updates", drop_pending_updates}};
    return call_api("deleteWebhook", payload, nullptr).has_value();
}

nlohmann::json TelegramAdapter::get_webhook_info() {
    auto res = call_api("getWebhookInfo", nlohmann::json::object(), nullptr);
    return res.value_or(nlohmann::json::object());
}

nlohmann::json TelegramAdapter::get_updates() {
    if (is_flood_waiting()) return nlohmann::json::array();
    nlohmann::json payload = {
        {"timeout", cfg_.getupdates_timeout_s},
        {"limit", cfg_.getupdates_limit},
    };
    if (next_update_offset_ != 0) payload["offset"] = next_update_offset_;
    TelegramError err;
    auto res = call_api("getUpdates", payload, &err);
    if (!res || !res->is_array()) {
        return nlohmann::json::array();
    }
    long long max_id = next_update_offset_;
    for (const auto& up : *res) {
        if (up.contains("update_id") && up["update_id"].is_number_integer()) {
            long long uid = up["update_id"].get<long long>();
            if (uid + 1 > max_id) max_id = uid + 1;
        }
    }
    next_update_offset_ = max_id;
    return *res;
}

// ---------------------------------------------------------------------------
// Approval state
// ---------------------------------------------------------------------------

long long TelegramAdapter::register_approval(std::string session_key) {
    std::lock_guard<std::mutex> lk(approval_mu_);
    long long id = ++approval_counter_;
    approval_state_[id] = std::move(session_key);
    return id;
}

std::optional<std::string> TelegramAdapter::take_approval(long long id) {
    std::lock_guard<std::mutex> lk(approval_mu_);
    auto it = approval_state_.find(id);
    if (it == approval_state_.end()) return std::nullopt;
    std::string s = std::move(it->second);
    approval_state_.erase(it);
    return s;
}

}  // namespace hermes::gateway::platforms
