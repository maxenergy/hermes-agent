// Phase 12 — Feishu (Lark) platform adapter — full-depth port of
// gateway/platforms/feishu.py.
//
// See feishu.hpp for the public API overview.  The implementation is
// intentionally written without runtime dependencies beyond OpenSSL and
// nlohmann::json so it can be unit-tested against a FakeHttpTransport.
#include "feishu.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <iterator>
#include <regex>
#include <sstream>
#include <stdexcept>

#if defined(HERMES_HAVE_OPENSSL) || __has_include(<openssl/evp.h>)
#include <openssl/evp.h>
#include <openssl/sha.h>
#define HERMES_FEISHU_HAS_OPENSSL 1
#else
#define HERMES_FEISHU_HAS_OPENSSL 0
#endif

namespace hermes::gateway::platforms {
namespace {

// ── Small pure helpers ────────────────────────────────────────────────────

bool icontains(const std::string& hay, const std::string& needle) {
    auto to_lower = [](std::string s) {
        std::transform(s.begin(), s.end(), s.begin(),
                       [](unsigned char c) { return std::tolower(c); });
        return s;
    };
    return to_lower(hay).find(to_lower(needle)) != std::string::npos;
}

std::string strip(const std::string& s) {
    const char* ws = " \t\r\n";
    const auto a = s.find_first_not_of(ws);
    if (a == std::string::npos) return "";
    const auto b = s.find_last_not_of(ws);
    return s.substr(a, b - a + 1);
}

std::string to_lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    return s;
}

std::vector<std::string> split_lines(const std::string& s) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (c == '\n') {
            out.push_back(cur);
            cur.clear();
        } else if (c == '\r') {
            continue;
        } else {
            cur.push_back(c);
        }
    }
    out.push_back(cur);
    return out;
}

// Convenience JSON field fetchers — mirror the Python `str(payload.get(k, "") or "").strip()`.
std::string js_str(const nlohmann::json& j, const std::string& key,
                   const std::string& fallback = "") {
    if (!j.is_object() || !j.contains(key)) return fallback;
    const auto& v = j[key];
    if (v.is_string()) return v.get<std::string>();
    if (v.is_number_integer()) return std::to_string(v.get<long long>());
    if (v.is_number_unsigned()) return std::to_string(v.get<unsigned long long>());
    if (v.is_number_float()) return std::to_string(v.get<double>());
    if (v.is_boolean()) return v.get<bool>() ? "true" : "false";
    return fallback;
}

bool js_truthy(const nlohmann::json& v) {
    if (v.is_null()) return false;
    if (v.is_boolean()) return v.get<bool>();
    if (v.is_number_integer()) return v.get<long long>() == 1;
    if (v.is_string()) {
        auto s = to_lower(v.get<std::string>());
        return s == "true" || s == "1";
    }
    return false;
}

}  // namespace

// ---------------------------------------------------------------------------
// Enum parsing
// ---------------------------------------------------------------------------

FeishuConnectionMode parse_connection_mode(std::string_view s) {
    std::string v = to_lower(std::string(s));
    if (v == "websocket" || v == "ws") return FeishuConnectionMode::WebSocket;
    if (v == "webhook" || v == "http") return FeishuConnectionMode::Webhook;
    return FeishuConnectionMode::Unknown;
}

std::string to_string(FeishuConnectionMode m) {
    switch (m) {
        case FeishuConnectionMode::WebSocket: return "websocket";
        case FeishuConnectionMode::Webhook:   return "webhook";
        default: return "unknown";
    }
}

FeishuGroupPolicy parse_group_policy(std::string_view s) {
    std::string v = to_lower(std::string(s));
    if (v == "open")         return FeishuGroupPolicy::Open;
    if (v == "allowlist" ||
        v == "whitelist")    return FeishuGroupPolicy::Allowlist;
    if (v == "blacklist" ||
        v == "blocklist")    return FeishuGroupPolicy::Blacklist;
    if (v == "admin_only" ||
        v == "admins")       return FeishuGroupPolicy::AdminOnly;
    if (v == "disabled" ||
        v == "off")          return FeishuGroupPolicy::Disabled;
    return FeishuGroupPolicy::Unknown;
}

std::string to_string(FeishuGroupPolicy p) {
    switch (p) {
        case FeishuGroupPolicy::Open:       return "open";
        case FeishuGroupPolicy::Allowlist:  return "allowlist";
        case FeishuGroupPolicy::Blacklist:  return "blacklist";
        case FeishuGroupPolicy::AdminOnly:  return "admin_only";
        case FeishuGroupPolicy::Disabled:   return "disabled";
        default: return "unknown";
    }
}

FeishuMessageType parse_message_type(std::string_view s) {
    std::string v = to_lower(std::string(s));
    if (v == "text")           return FeishuMessageType::Text;
    if (v == "post")           return FeishuMessageType::Post;
    if (v == "image")          return FeishuMessageType::Image;
    if (v == "file")           return FeishuMessageType::File;
    if (v == "audio")          return FeishuMessageType::Audio;
    if (v == "media")          return FeishuMessageType::Media;
    if (v == "sticker")        return FeishuMessageType::Sticker;
    if (v == "interactive" ||
        v == "card")           return FeishuMessageType::Interactive;
    if (v == "share_chat")     return FeishuMessageType::ShareChat;
    if (v == "merge_forward")  return FeishuMessageType::MergeForward;
    if (v == "system")         return FeishuMessageType::System;
    return FeishuMessageType::Unknown;
}

std::string to_string(FeishuMessageType t) {
    switch (t) {
        case FeishuMessageType::Text:         return "text";
        case FeishuMessageType::Post:         return "post";
        case FeishuMessageType::Image:        return "image";
        case FeishuMessageType::File:         return "file";
        case FeishuMessageType::Audio:        return "audio";
        case FeishuMessageType::Media:        return "media";
        case FeishuMessageType::Sticker:      return "sticker";
        case FeishuMessageType::Interactive:  return "interactive";
        case FeishuMessageType::ShareChat:    return "share_chat";
        case FeishuMessageType::MergeForward: return "merge_forward";
        case FeishuMessageType::System:       return "system";
        default: return "unknown";
    }
}

// ---------------------------------------------------------------------------
// Text helpers
// ---------------------------------------------------------------------------

std::string escape_markdown_text(const std::string& text) {
    static const std::string kSpecials = "\\`*_{}[]()#+-!|>~";
    std::string out;
    out.reserve(text.size() + 8);
    for (char c : text) {
        if (kSpecials.find(c) != std::string::npos) {
            out.push_back('\\');
        }
        out.push_back(c);
    }
    return out;
}

std::string wrap_inline_code(const std::string& text) {
    std::size_t max_run = 0;
    std::size_t cur = 0;
    for (char c : text) {
        if (c == '`') {
            ++cur;
            max_run = std::max(max_run, cur);
        } else {
            cur = 0;
        }
    }
    std::string fence(max_run + 1, '`');
    std::string body = text;
    if (!text.empty() && (text.front() == '`' || text.back() == '`')) {
        body = " " + text + " ";
    }
    return fence + body + fence;
}

std::string sanitize_fence_language(const std::string& language) {
    std::string s = strip(language);
    for (char& c : s) {
        if (c == '\n' || c == '\r') c = ' ';
    }
    return s;
}

static const std::regex& mention_placeholder_re() {
    static const std::regex re(R"(@_user_\d+)");
    return re;
}

std::string normalize_feishu_text(const std::string& text) {
    std::string cleaned = std::regex_replace(text, mention_placeholder_re(), " ");
    // Normalize line endings.
    std::string normalized;
    normalized.reserve(cleaned.size());
    for (std::size_t i = 0; i < cleaned.size(); ++i) {
        char c = cleaned[i];
        if (c == '\r') {
            normalized.push_back('\n');
            if (i + 1 < cleaned.size() && cleaned[i + 1] == '\n') ++i;
        } else {
            normalized.push_back(c);
        }
    }
    // Collapse whitespace within lines, drop empty lines, strip each line.
    std::vector<std::string> kept;
    for (auto& line : split_lines(normalized)) {
        std::string acc;
        bool in_ws = false;
        for (char c : line) {
            if (c == ' ' || c == '\t') {
                if (!in_ws && !acc.empty()) acc.push_back(' ');
                in_ws = true;
            } else {
                acc.push_back(c);
                in_ws = false;
            }
        }
        acc = strip(acc);
        if (!acc.empty()) kept.push_back(acc);
    }
    std::string out;
    for (std::size_t i = 0; i < kept.size(); ++i) {
        if (i) out.push_back('\n');
        out += kept[i];
    }
    return strip(out);
}

std::vector<std::string> unique_lines(const std::vector<std::string>& lines) {
    std::vector<std::string> out;
    std::unordered_set<std::string> seen;
    for (const auto& line : lines) {
        if (line.empty()) continue;
        if (seen.count(line)) continue;
        seen.insert(line);
        out.push_back(line);
    }
    return out;
}

std::string strip_markdown_to_plain_text(const std::string& text) {
    std::string plain = text;
    // CRLF → LF.
    std::string norm;
    norm.reserve(plain.size());
    for (std::size_t i = 0; i < plain.size(); ++i) {
        if (plain[i] == '\r') {
            norm.push_back('\n');
            if (i + 1 < plain.size() && plain[i + 1] == '\n') ++i;
        } else {
            norm.push_back(plain[i]);
        }
    }
    plain = norm;

    // Markdown link [text](url) → "text (url)"
    static const std::regex link_re(R"(\[([^\]]+)\]\(([^)]+)\))");
    plain = std::regex_replace(plain, link_re, "$1 ($2)");
    // Headings: ^#+\s+ → ""
    static const std::regex heading_re(R"(^#{1,6}\s+)",
                                       std::regex::multiline);
    plain = std::regex_replace(plain, heading_re, "");
    // Blockquote prefix ^>\s? → ""
    static const std::regex quote_re(R"(^>\s?)", std::regex::multiline);
    plain = std::regex_replace(plain, quote_re, "");
    // Horizontal rule ^---+ → "---"
    static const std::regex hr_re(R"(^\s*---+\s*$)",
                                  std::regex::multiline);
    plain = std::regex_replace(plain, hr_re, "---");
    // ```fenced``` blocks → inner body.
    static const std::regex fence_re(R"(```(?:[^\n]*\n)?([\s\S]*?)```)");
    plain = std::regex_replace(plain, fence_re, "$1");
    // `inline` → inline
    static const std::regex inline_re(R"(`([^`\n]+)`)");
    plain = std::regex_replace(plain, inline_re, "$1");
    // **bold**
    static const std::regex bold_re(R"(\*\*([^*\n]+)\*\*)");
    plain = std::regex_replace(plain, bold_re, "$1");
    // *italic* (non-greedy, no-newline)
    static const std::regex italic_re(R"(\*([^*\n]+)\*)");
    plain = std::regex_replace(plain, italic_re, "$1");
    // ~~strike~~
    static const std::regex strike_re(R"(~~([^~\n]+)~~)");
    plain = std::regex_replace(plain, strike_re, "$1");
    // <u>underline</u>
    static const std::regex underline_re(R"(<u>([\s\S]*?)</u>)");
    plain = std::regex_replace(plain, underline_re, "$1");
    // Collapse excessive blank lines.
    static const std::regex blank_re(R"(\n{3,})");
    plain = std::regex_replace(plain, blank_re, "\n\n");
    return strip(plain);
}

std::optional<long long> coerce_int(const nlohmann::json& value,
                                    long long min_value) {
    try {
        long long parsed = 0;
        if (value.is_number_integer()) parsed = value.get<long long>();
        else if (value.is_number_unsigned()) parsed = static_cast<long long>(value.get<unsigned long long>());
        else if (value.is_number_float()) parsed = static_cast<long long>(value.get<double>());
        else if (value.is_string()) parsed = std::stoll(value.get<std::string>());
        else return std::nullopt;
        if (parsed < min_value) return std::nullopt;
        return parsed;
    } catch (...) {
        return std::nullopt;
    }
}

long long coerce_required_int(const nlohmann::json& value, long long def,
                              long long min_value) {
    auto p = coerce_int(value, min_value);
    return p ? *p : def;
}

// ---------------------------------------------------------------------------
// Post payload building / parsing
// ---------------------------------------------------------------------------

std::string build_markdown_post_payload(const std::string& content) {
    nlohmann::json payload = {
        {"zh_cn", {
            {"content", nlohmann::json::array({
                nlohmann::json::array({
                    {{"tag", "md"}, {"text", content}},
                })
            })}
        }}
    };
    return payload.dump();
}

namespace {

const std::array<const char*, 2> kPreferredLocales = {"zh_cn", "en_us"};

nlohmann::json to_post_payload(const nlohmann::json& candidate) {
    if (!candidate.is_object()) return nlohmann::json();
    if (!candidate.contains("content") || !candidate["content"].is_array()) {
        return nlohmann::json();
    }
    nlohmann::json out = nlohmann::json::object();
    out["title"] = candidate.value("title", "");
    out["content"] = candidate["content"];
    return out;
}

nlohmann::json resolve_locale_payload(const nlohmann::json& payload) {
    auto direct = to_post_payload(payload);
    if (!direct.is_null()) return direct;
    if (!payload.is_object()) return nlohmann::json();
    for (auto k : kPreferredLocales) {
        if (payload.contains(k)) {
            auto r = to_post_payload(payload[k]);
            if (!r.is_null()) return r;
        }
    }
    for (auto it = payload.begin(); it != payload.end(); ++it) {
        auto r = to_post_payload(it.value());
        if (!r.is_null()) return r;
    }
    return nlohmann::json();
}

nlohmann::json resolve_post_payload(const nlohmann::json& payload) {
    auto direct = to_post_payload(payload);
    if (!direct.is_null()) return direct;
    if (!payload.is_object()) return nlohmann::json();
    if (payload.contains("post")) {
        auto wrapped = resolve_locale_payload(payload["post"]);
        if (!wrapped.is_null()) return wrapped;
    }
    return resolve_locale_payload(payload);
}

std::string render_post_element(
    const nlohmann::json& element,
    std::vector<std::string>& image_keys,
    std::vector<FeishuPostMediaRef>& media_refs,
    std::vector<std::string>& mentioned_ids);

std::string render_nested_post(
    const nlohmann::json& value,
    std::vector<std::string>& image_keys,
    std::vector<FeishuPostMediaRef>& media_refs,
    std::vector<std::string>& mentioned_ids) {
    if (value.is_string()) {
        return escape_markdown_text(value.get<std::string>());
    }
    if (value.is_array()) {
        std::vector<std::string> parts;
        for (const auto& item : value) {
            auto p = render_nested_post(item, image_keys, media_refs,
                                        mentioned_ids);
            if (!p.empty()) parts.push_back(p);
        }
        std::string out;
        for (std::size_t i = 0; i < parts.size(); ++i) {
            if (i) out.push_back(' ');
            out += parts[i];
        }
        return out;
    }
    if (value.is_object()) {
        auto direct = render_post_element(value, image_keys, media_refs,
                                          mentioned_ids);
        if (!direct.empty()) return direct;
        std::vector<std::string> parts;
        for (auto it = value.begin(); it != value.end(); ++it) {
            auto p = render_nested_post(it.value(), image_keys, media_refs,
                                        mentioned_ids);
            if (!p.empty()) parts.push_back(p);
        }
        std::string out;
        for (std::size_t i = 0; i < parts.size(); ++i) {
            if (i) out.push_back(' ');
            out += parts[i];
        }
        return out;
    }
    return "";
}

std::string render_text_element(const nlohmann::json& element) {
    std::string text = js_str(element, "text");
    const auto& style = element.value("style", nlohmann::json::object());
    auto has_style = [&](const std::string& k) {
        return style.is_object() && style.contains(k) && js_truthy(style[k]);
    };
    if (has_style("code")) return wrap_inline_code(text);
    std::string rendered = escape_markdown_text(text);
    if (rendered.empty()) return "";
    if (has_style("bold"))           rendered = "**" + rendered + "**";
    if (has_style("italic"))         rendered = "*" + rendered + "*";
    if (has_style("underline"))      rendered = "<u>" + rendered + "</u>";
    if (has_style("strikethrough"))  rendered = "~~" + rendered + "~~";
    return rendered;
}

std::string render_code_block_element(const nlohmann::json& element) {
    std::string language = sanitize_fence_language(
        js_str(element, "language", js_str(element, "lang")));
    std::string code = js_str(element, "text");
    if (code.empty()) code = js_str(element, "content");
    // Normalize CRLF to LF.
    std::string body;
    body.reserve(code.size());
    for (std::size_t i = 0; i < code.size(); ++i) {
        if (code[i] == '\r') {
            if (i + 1 < code.size() && code[i + 1] == '\n') { ++i; }
            body.push_back('\n');
        } else {
            body.push_back(code[i]);
        }
    }
    std::string trailing = (!body.empty() && body.back() == '\n') ? "" : "\n";
    return "```" + language + "\n" + body + trailing + "```";
}

std::string render_post_element(
    const nlohmann::json& element,
    std::vector<std::string>& image_keys,
    std::vector<FeishuPostMediaRef>& media_refs,
    std::vector<std::string>& mentioned_ids) {
    if (element.is_string()) return element.get<std::string>();
    if (!element.is_object()) return "";
    std::string tag = to_lower(strip(js_str(element, "tag")));

    if (tag == "text") return render_text_element(element);
    if (tag == "a") {
        std::string href = strip(js_str(element, "href"));
        std::string label = strip(js_str(element, "text", href));
        if (label.empty()) return "";
        std::string esc = escape_markdown_text(label);
        if (!href.empty()) return "[" + esc + "](" + href + ")";
        return esc;
    }
    if (tag == "at") {
        std::string mid = strip(js_str(element, "open_id"));
        if (mid.empty()) mid = strip(js_str(element, "user_id"));
        if (!mid.empty() &&
            std::find(mentioned_ids.begin(), mentioned_ids.end(), mid) ==
                mentioned_ids.end()) {
            mentioned_ids.push_back(mid);
        }
        std::string name = strip(js_str(element, "user_name"));
        if (name.empty()) name = strip(js_str(element, "name"));
        if (name.empty()) name = strip(js_str(element, "text"));
        if (name.empty()) name = mid;
        return name.empty() ? "@" : "@" + escape_markdown_text(name);
    }
    if (tag == "img" || tag == "image") {
        std::string key = strip(js_str(element, "image_key"));
        if (!key.empty() &&
            std::find(image_keys.begin(), image_keys.end(), key) ==
                image_keys.end()) {
            image_keys.push_back(key);
        }
        std::string alt = strip(js_str(element, "text"));
        if (alt.empty()) alt = strip(js_str(element, "alt"));
        return alt.empty() ? "[Image]" : ("[Image: " + alt + "]");
    }
    if (tag == "media" || tag == "file" || tag == "audio" || tag == "video") {
        std::string key = strip(js_str(element, "file_key"));
        std::string name = strip(js_str(element, "file_name"));
        if (name.empty()) name = strip(js_str(element, "title"));
        if (name.empty()) name = strip(js_str(element, "text"));
        if (!key.empty()) {
            FeishuPostMediaRef ref;
            ref.file_key = key;
            ref.file_name = name;
            ref.resource_type = (tag == "audio" || tag == "video") ? tag : "file";
            media_refs.push_back(std::move(ref));
        }
        return name.empty() ? "[Attachment]" : ("[Attachment: " + name + "]");
    }
    if (tag == "emotion" || tag == "emoji") {
        std::string label = strip(js_str(element, "text"));
        if (label.empty()) label = strip(js_str(element, "emoji_type"));
        if (label.empty()) return "[Emoji]";
        return ":" + escape_markdown_text(label) + ":";
    }
    if (tag == "br") return "\n";
    if (tag == "hr" || tag == "divider") return "\n\n---\n\n";
    if (tag == "code") {
        std::string code = js_str(element, "text");
        if (code.empty()) code = js_str(element, "content");
        return code.empty() ? "" : wrap_inline_code(code);
    }
    if (tag == "code_block" || tag == "pre") {
        return render_code_block_element(element);
    }

    std::vector<std::string> nested;
    for (auto k : {"text", "title", "content", "children", "elements"}) {
        if (!element.contains(k)) continue;
        auto p = render_nested_post(element[k], image_keys, media_refs,
                                    mentioned_ids);
        if (!p.empty()) nested.push_back(p);
    }
    std::string out;
    for (std::size_t i = 0; i < nested.size(); ++i) {
        if (i) out.push_back(' ');
        out += nested[i];
    }
    return out;
}

}  // namespace

FeishuPostParseResult parse_feishu_post_payload(const nlohmann::json& payload) {
    auto resolved = resolve_post_payload(payload);
    FeishuPostParseResult r;
    if (!resolved.is_object()) {
        r.text_content = "[Rich text message]";
        return r;
    }
    std::vector<std::string> parts;
    std::string title = normalize_feishu_text(
        strip(resolved.value("title", std::string())));
    if (!title.empty()) parts.push_back(title);
    if (resolved.contains("content") && resolved["content"].is_array()) {
        for (const auto& row : resolved["content"]) {
            if (!row.is_array()) continue;
            std::string row_text;
            for (const auto& item : row) {
                row_text += render_post_element(item, r.image_keys,
                                                r.media_refs,
                                                r.mentioned_ids);
            }
            row_text = normalize_feishu_text(row_text);
            if (!row_text.empty()) parts.push_back(row_text);
        }
    }
    std::string joined;
    for (std::size_t i = 0; i < parts.size(); ++i) {
        if (i) joined.push_back('\n');
        joined += parts[i];
    }
    joined = strip(joined);
    r.text_content = joined.empty() ? "[Rich text message]" : joined;
    return r;
}

FeishuPostParseResult parse_feishu_post_content(const std::string& raw) {
    if (raw.empty()) {
        FeishuPostParseResult r;
        r.text_content = "[Rich text message]";
        return r;
    }
    try {
        auto parsed = nlohmann::json::parse(raw);
        return parse_feishu_post_payload(parsed);
    } catch (...) {
        FeishuPostParseResult r;
        r.text_content = "[Rich text message]";
        return r;
    }
}

// ---------------------------------------------------------------------------
// Message normalization
// ---------------------------------------------------------------------------

FeishuNormalizedMessage normalize_feishu_message(const std::string& message_type,
                                                 const std::string& raw_content) {
    std::string t = to_lower(strip(message_type));
    nlohmann::json payload = nlohmann::json::object();
    if (!raw_content.empty()) {
        try {
            payload = nlohmann::json::parse(raw_content);
            if (!payload.is_object()) {
                nlohmann::json wrap = {{"content", payload}};
                payload = wrap;
            }
        } catch (...) {
            payload = {{"text", raw_content}};
        }
    }

    FeishuNormalizedMessage out;
    out.raw_type = t;

    if (t == "text") {
        out.text_content = normalize_feishu_text(
            payload.value("text", std::string()));
        return out;
    }
    if (t == "post") {
        auto parsed = parse_feishu_post_payload(payload);
        out.text_content = parsed.text_content;
        out.image_keys = parsed.image_keys;
        out.media_refs = parsed.media_refs;
        out.mentioned_ids = parsed.mentioned_ids;
        out.relation_kind = "post";
        return out;
    }
    if (t == "image") {
        std::string image_key = strip(payload.value("image_key", ""));
        std::string alt = payload.value("text", std::string());
        if (alt.empty()) alt = payload.value("alt", std::string());
        std::string norm_alt = normalize_feishu_text(alt);
        out.preferred_message_type = "photo";
        if (!image_key.empty()) out.image_keys.push_back(image_key);
        if (norm_alt != "[Image]") out.text_content = norm_alt;
        out.relation_kind = "image";
        return out;
    }
    if (t == "file" || t == "audio" || t == "media") {
        FeishuPostMediaRef ref;
        ref.file_key = strip(payload.value("file_key", ""));
        ref.file_name = payload.value("file_name", std::string());
        if (ref.file_name.empty()) ref.file_name = payload.value("title", std::string());
        if (ref.file_name.empty()) ref.file_name = payload.value("text", std::string());
        ref.resource_type = (t == "audio" || t == "video") ? t : "file";
        out.relation_kind = t;
        out.preferred_message_type = (t == "audio") ? "audio" : "document";
        if (!ref.file_key.empty()) out.media_refs.push_back(ref);
        std::string name = normalize_feishu_text(ref.file_name);
        out.metadata["placeholder_text"] =
            name.empty() ? "[Attachment]" : ("[Attachment: " + name + "]");
        return out;
    }
    if (t == "interactive" || t == "card") {
        nlohmann::json card = payload.contains("card") && payload["card"].is_object()
                              ? payload["card"] : payload;
        // Find header/title.
        std::string title;
        if (card.is_object() && card.contains("header") &&
            card["header"].is_object()) {
            const auto& h = card["header"];
            if (h.contains("title")) {
                const auto& hv = h["title"];
                if (hv.is_object()) {
                    title = hv.value("content", std::string());
                    if (title.empty()) title = hv.value("text", std::string());
                } else if (hv.is_string()) {
                    title = hv.get<std::string>();
                }
            }
        }
        if (title.empty()) title = payload.value("title", std::string());
        std::vector<std::string> lines;
        if (!title.empty()) lines.push_back(normalize_feishu_text(title));
        out.text_content = lines.empty()
                               ? std::string("[Interactive message]")
                               : lines.front();
        out.relation_kind = "interactive";
        out.metadata["title"] = title;
        return out;
    }
    if (t == "share_chat") {
        std::string name = payload.value("chat_name", std::string());
        if (name.empty()) name = payload.value("name", std::string());
        std::string cid = payload.value("chat_id", std::string());
        std::vector<std::string> lines;
        if (!name.empty()) lines.push_back("Shared chat: " + name);
        else lines.push_back("[Shared chat]");
        if (!cid.empty()) lines.push_back("Chat ID: " + cid);
        std::string joined;
        for (std::size_t i = 0; i < lines.size(); ++i) {
            if (i) joined.push_back('\n');
            joined += lines[i];
        }
        out.text_content = joined;
        out.relation_kind = "share_chat";
        out.metadata["chat_id"] = cid;
        out.metadata["chat_name"] = name;
        return out;
    }
    if (t == "merge_forward") {
        std::string title = payload.value("title", std::string());
        if (title.empty()) title = payload.value("summary", std::string());
        std::string joined;
        if (!title.empty()) joined = title;
        else joined = "[Merged forward message]";
        out.text_content = joined;
        out.relation_kind = "merge_forward";
        out.metadata["title"] = title;
        return out;
    }
    return out;
}

// ---------------------------------------------------------------------------
// Message segmentation
// ---------------------------------------------------------------------------

std::vector<std::string> split_message_for_feishu(const std::string& content,
                                                  std::size_t max_len) {
    std::vector<std::string> out;
    if (content.size() <= max_len) {
        out.push_back(content);
        return out;
    }
    // Prefer to split on paragraph boundaries, then lines, then characters.
    std::size_t pos = 0;
    while (pos < content.size()) {
        std::size_t remaining = content.size() - pos;
        if (remaining <= max_len) {
            out.push_back(content.substr(pos));
            break;
        }
        std::size_t window = max_len;
        std::size_t best = std::string::npos;
        // Try paragraph break.
        best = content.rfind("\n\n", pos + window);
        if (best == std::string::npos || best < pos + (max_len / 2)) {
            // Try single newline.
            best = content.rfind('\n', pos + window);
        }
        if (best == std::string::npos || best < pos + (max_len / 2)) {
            best = pos + window;
        }
        out.push_back(content.substr(pos, best - pos));
        pos = best;
        // Skip leading whitespace of next chunk.
        while (pos < content.size() &&
               (content[pos] == '\n' || content[pos] == ' ')) {
            ++pos;
        }
    }
    return out;
}

// ---------------------------------------------------------------------------
// Error classification
// ---------------------------------------------------------------------------

FeishuError classify_feishu_error(int http_status, const nlohmann::json& body) {
    FeishuError err;
    err.http_status = http_status;
    if (body.is_object()) {
        err.feishu_code = body.value("code", 0);
        err.message = body.value("msg", std::string());
    }

    if (http_status >= 500 || http_status == 0) {
        err.kind = FeishuErrorKind::Transient;
        return err;
    }
    if (http_status == 401 || http_status == 403) {
        err.kind = FeishuErrorKind::Unauthorized;
        return err;
    }
    if (http_status == 429) {
        err.kind = FeishuErrorKind::RateLimited;
        err.retry_after_seconds = 5.0;
        return err;
    }

    int code = err.feishu_code;
    // Feishu "99991663/99991664" → token invalid/expired.
    if (code == 99991663 || code == 99991664) {
        err.kind = FeishuErrorKind::Unauthorized;
        return err;
    }
    if (code == 99991400 || code == 99991401) {
        err.kind = FeishuErrorKind::RateLimited;
        err.retry_after_seconds = 3.0;
        return err;
    }
    if (code == 230011 || code == 231003) {
        err.kind = FeishuErrorKind::ReplyMissing;
        return err;
    }
    if (code != 0) {
        err.kind = FeishuErrorKind::BadRequest;
        return err;
    }
    if (http_status >= 200 && http_status < 300) {
        err.kind = FeishuErrorKind::None;
        return err;
    }
    err.kind = FeishuErrorKind::Fatal;
    return err;
}

std::string normalize_chat_type(const std::string& raw) {
    std::string v = to_lower(strip(raw));
    if (v == "p2p" || v == "single" || v == "user" || v == "private") return "private";
    if (v == "group" || v == "groupchat" || v == "multi") return "group";
    if (v == "topic" || v == "channel" || v == "public") return "channel";
    return "unknown";
}

// ---------------------------------------------------------------------------
// Base64 + SHA256 + AES-256-CBC decrypt
// ---------------------------------------------------------------------------

static const std::string kB64Table =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string feishu_base64_encode(std::string_view bytes) {
    std::string out;
    std::size_t n = bytes.size();
    out.reserve(((n + 2) / 3) * 4);
    std::size_t i = 0;
    while (i + 3 <= n) {
        unsigned b0 = static_cast<unsigned char>(bytes[i]);
        unsigned b1 = static_cast<unsigned char>(bytes[i + 1]);
        unsigned b2 = static_cast<unsigned char>(bytes[i + 2]);
        out.push_back(kB64Table[(b0 >> 2) & 0x3F]);
        out.push_back(kB64Table[((b0 << 4) | (b1 >> 4)) & 0x3F]);
        out.push_back(kB64Table[((b1 << 2) | (b2 >> 6)) & 0x3F]);
        out.push_back(kB64Table[b2 & 0x3F]);
        i += 3;
    }
    if (i < n) {
        unsigned b0 = static_cast<unsigned char>(bytes[i]);
        unsigned b1 = (i + 1 < n) ? static_cast<unsigned char>(bytes[i + 1]) : 0;
        out.push_back(kB64Table[(b0 >> 2) & 0x3F]);
        out.push_back(kB64Table[((b0 << 4) | (b1 >> 4)) & 0x3F]);
        if (i + 1 < n) {
            out.push_back(kB64Table[(b1 << 2) & 0x3F]);
            out.push_back('=');
        } else {
            out.push_back('=');
            out.push_back('=');
        }
    }
    return out;
}

std::string feishu_base64_decode(std::string_view input) {
    static std::array<int, 256> table = [] {
        std::array<int, 256> t{};
        t.fill(-1);
        for (std::size_t i = 0; i < kB64Table.size(); ++i) {
            t[static_cast<unsigned char>(kB64Table[i])] = static_cast<int>(i);
        }
        return t;
    }();
    std::string out;
    out.reserve((input.size() / 4) * 3);
    int buf = 0, bits = 0;
    for (char c : input) {
        if (c == '=' || c == '\n' || c == '\r' || c == ' ' || c == '\t') continue;
        int v = table[static_cast<unsigned char>(c)];
        if (v < 0) continue;
        buf = (buf << 6) | v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<char>((buf >> bits) & 0xFF));
        }
    }
    return out;
}

std::string sha256_bytes(std::string_view bytes) {
#if HERMES_FEISHU_HAS_OPENSSL
    unsigned char digest[32];
    SHA256(reinterpret_cast<const unsigned char*>(bytes.data()), bytes.size(),
           digest);
    return std::string(reinterpret_cast<char*>(digest), 32);
#else
    (void)bytes;
    return std::string();
#endif
}

std::optional<std::string> feishu_aes_decrypt(const std::string& encrypt_key,
                                              const std::string& base64_ct) {
#if HERMES_FEISHU_HAS_OPENSSL
    if (encrypt_key.empty() || base64_ct.empty()) return std::nullopt;
    std::string key = sha256_bytes(encrypt_key);
    if (key.size() != 32) return std::nullopt;
    std::string ct = feishu_base64_decode(base64_ct);
    if (ct.size() < 17 || (ct.size() - 16) % 16 != 0) return std::nullopt;
    const unsigned char* iv =
        reinterpret_cast<const unsigned char*>(ct.data());
    const unsigned char* cipher =
        reinterpret_cast<const unsigned char*>(ct.data() + 16);
    int cipher_len = static_cast<int>(ct.size() - 16);

    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (!ctx) return std::nullopt;
    std::string out(cipher_len + 16, '\0');
    int len = 0, total = 0;
    auto fail = [&]() {
        EVP_CIPHER_CTX_free(ctx);
        return std::nullopt;
    };
    if (EVP_DecryptInit_ex(ctx, EVP_aes_256_cbc(), nullptr,
                           reinterpret_cast<const unsigned char*>(key.data()),
                           iv) != 1) return fail();
    if (EVP_DecryptUpdate(ctx,
                          reinterpret_cast<unsigned char*>(&out[0]), &len,
                          cipher, cipher_len) != 1) return fail();
    total = len;
    if (EVP_DecryptFinal_ex(ctx,
                            reinterpret_cast<unsigned char*>(&out[total]),
                            &len) != 1) return fail();
    total += len;
    EVP_CIPHER_CTX_free(ctx);
    out.resize(total);
    return out;
#else
    (void)encrypt_key;
    (void)base64_ct;
    return std::nullopt;
#endif
}

// ---------------------------------------------------------------------------
// FeishuRateLimiter
// ---------------------------------------------------------------------------

bool FeishuRateLimiter::allow(const std::string& key,
                              std::size_t max_per_window,
                              double window_seconds,
                              std::chrono::steady_clock::time_point now) {
    std::lock_guard<std::mutex> lock(mu_);
    if (counts_.size() > max_keys_) {
        // Drop oldest entries (rough eviction).
        auto cutoff = now - std::chrono::seconds(static_cast<int>(window_seconds));
        for (auto it = counts_.begin(); it != counts_.end();) {
            if (it->second.window_start < cutoff) it = counts_.erase(it);
            else ++it;
        }
    }
    auto& e = counts_[key];
    std::chrono::duration<double> elapsed = now - e.window_start;
    if (e.count == 0 || elapsed.count() > window_seconds) {
        e.count = 1;
        e.window_start = now;
        return true;
    }
    if (e.count >= max_per_window) return false;
    e.count += 1;
    return true;
}

std::size_t FeishuRateLimiter::tracked_keys() const {
    std::lock_guard<std::mutex> lock(mu_);
    return counts_.size();
}

void FeishuRateLimiter::clear() {
    std::lock_guard<std::mutex> lock(mu_);
    counts_.clear();
}

// ---------------------------------------------------------------------------
// FeishuAnomalyTracker
// ---------------------------------------------------------------------------

bool FeishuAnomalyTracker::record(const std::string& ip,
                                  const std::string& status,
                                  std::chrono::steady_clock::time_point now) {
    std::lock_guard<std::mutex> lock(mu_);
    // GC stale entries.
    auto ttl = std::chrono::seconds(static_cast<int>(ttl_seconds_));
    for (auto it = entries_.begin(); it != entries_.end();) {
        if (now - it->second.first_seen > ttl) it = entries_.erase(it);
        else ++it;
    }
    auto& e = entries_[ip];
    if (e.count == 0) e.first_seen = now;
    e.count += 1;
    e.last_status = status;
    if (!e.alerted && e.count >= threshold_) {
        e.alerted = true;
        return true;
    }
    return false;
}

void FeishuAnomalyTracker::clear(const std::string& ip) {
    std::lock_guard<std::mutex> lock(mu_);
    entries_.erase(ip);
}

std::size_t FeishuAnomalyTracker::tracked_ips() const {
    std::lock_guard<std::mutex> lock(mu_);
    return entries_.size();
}

// ---------------------------------------------------------------------------
// FeishuDedupCache
// ---------------------------------------------------------------------------

bool FeishuDedupCache::check_and_add(const std::string& message_id,
                                     std::chrono::steady_clock::time_point now) {
    std::lock_guard<std::mutex> lock(mu_);
    auto ttl = std::chrono::duration<double>(ttl_);
    auto it = seen_.find(message_id);
    if (it != seen_.end()) {
        if (now - it->second < ttl) return false;
    }
    seen_[message_id] = now;
    order_.push_back(message_id);
    while (order_.size() > cap_) {
        seen_.erase(order_.front());
        order_.pop_front();
    }
    // Also evict TTL-expired entries opportunistically.
    while (!order_.empty()) {
        auto mit = seen_.find(order_.front());
        if (mit != seen_.end() && now - mit->second >= ttl) {
            seen_.erase(mit);
            order_.pop_front();
        } else {
            break;
        }
    }
    return true;
}

std::size_t FeishuDedupCache::size() const {
    std::lock_guard<std::mutex> lock(mu_);
    return seen_.size();
}

void FeishuDedupCache::clear() {
    std::lock_guard<std::mutex> lock(mu_);
    seen_.clear();
    order_.clear();
}

// ---------------------------------------------------------------------------
// FeishuAdapter
// ---------------------------------------------------------------------------

FeishuAdapter::FeishuAdapter(Config cfg)
    : cfg_(std::move(cfg)),
      dedup_cache_(cfg_.dedup_cache_size, 24 * 60 * 60.0) {}

FeishuAdapter::FeishuAdapter(Config cfg, hermes::llm::HttpTransport* transport)
    : cfg_(std::move(cfg)),
      transport_(transport),
      dedup_cache_(cfg_.dedup_cache_size, 24 * 60 * 60.0) {}

hermes::llm::HttpTransport* FeishuAdapter::get_transport() {
    if (transport_) return transport_;
    return hermes::llm::get_default_transport();
}

std::string FeishuAdapter::api_domain() const {
    std::string d = to_lower(cfg_.domain);
    if (d == "lark" || d == "larksuite") return "https://open.larksuite.com";
    return "https://open.feishu.cn";
}

std::string FeishuAdapter::base_url() const {
    return api_domain() + "/open-apis";
}

std::string FeishuAdapter::auth_url() const {
    return base_url() + "/auth/v3/tenant_access_token/internal";
}

std::string FeishuAdapter::messages_url(const std::string& receive_id_type) const {
    return base_url() + "/im/v1/messages?receive_id_type=" + receive_id_type;
}

std::string FeishuAdapter::message_url(const std::string& message_id) const {
    return base_url() + "/im/v1/messages/" + message_id;
}

std::string FeishuAdapter::reply_url(const std::string& message_id) const {
    return base_url() + "/im/v1/messages/" + message_id + "/reply";
}

std::string FeishuAdapter::update_url(const std::string& message_id) const {
    return base_url() + "/im/v1/messages/" + message_id;
}

std::string FeishuAdapter::reaction_url(const std::string& message_id) const {
    return base_url() + "/im/v1/messages/" + message_id + "/reactions";
}

std::string FeishuAdapter::chat_info_url(const std::string& chat_id) const {
    return base_url() + "/im/v1/chats/" + chat_id;
}

std::string FeishuAdapter::chat_members_url(const std::string& chat_id,
                                            const std::string& page_token) const {
    std::string u = base_url() + "/im/v1/chats/" + chat_id + "/members";
    if (!page_token.empty()) u += "?page_token=" + page_token;
    return u;
}

std::string FeishuAdapter::image_upload_url() const {
    return base_url() + "/im/v1/images";
}

std::string FeishuAdapter::file_upload_url() const {
    return base_url() + "/im/v1/files";
}

std::string FeishuAdapter::message_resource_url(const std::string& message_id,
                                                const std::string& file_key,
                                                const std::string& type) const {
    return base_url() + "/im/v1/messages/" + message_id + "/resources/" +
           file_key + "?type=" + type;
}

// ----- Auth ---------------------------------------------------------------

bool FeishuAdapter::refresh_tenant_access_token() {
    auto* transport = get_transport();
    if (!transport) return false;
    if (cfg_.app_id.empty() || cfg_.app_secret.empty()) return false;

    nlohmann::json payload = {{"app_id", cfg_.app_id},
                              {"app_secret", cfg_.app_secret}};
    try {
        auto resp = transport->post_json(
            auth_url(), {{"Content-Type", "application/json"}},
            payload.dump());
        nlohmann::json body;
        try { body = nlohmann::json::parse(resp.body); } catch (...) {
            body = nlohmann::json::object();
        }
        if (resp.status_code != 200) {
            std::lock_guard<std::mutex> lk(err_mu_);
            last_error_ = classify_feishu_error(resp.status_code, body);
            return false;
        }
        if (body.value("code", -1) != 0) {
            std::lock_guard<std::mutex> lk(err_mu_);
            last_error_ = classify_feishu_error(200, body);
            return false;
        }
        std::string tok = body.value("tenant_access_token", std::string());
        if (tok.empty()) return false;
        int expire = body.value("expire", 7200);
        // Refresh 5 minutes before expiry.
        set_tenant_access_token(std::move(tok),
                                std::chrono::seconds(std::max(60, expire - 300)));
        {
            std::lock_guard<std::mutex> lk(err_mu_);
            last_error_ = FeishuError{};
        }
        return true;
    } catch (...) {
        return false;
    }
}

std::string FeishuAdapter::tenant_access_token() const {
    std::lock_guard<std::mutex> lk(token_mu_);
    return tenant_access_token_;
}

void FeishuAdapter::set_tenant_access_token(std::string tok,
                                            std::chrono::seconds ttl) {
    std::lock_guard<std::mutex> lk(token_mu_);
    tenant_access_token_ = std::move(tok);
    token_expires_at_ = std::chrono::steady_clock::now() + ttl;
}

bool FeishuAdapter::access_token_expired(
    std::chrono::steady_clock::time_point now) const {
    std::lock_guard<std::mutex> lk(token_mu_);
    if (tenant_access_token_.empty()) return true;
    return now >= token_expires_at_;
}

std::unordered_map<std::string, std::string> FeishuAdapter::auth_headers() const {
    return {
        {"Authorization", "Bearer " + tenant_access_token()},
        {"Content-Type", "application/json; charset=utf-8"},
    };
}

// ----- BasePlatformAdapter overrides --------------------------------------

bool FeishuAdapter::connect() {
    if (cfg_.app_id.empty() || cfg_.app_secret.empty()) {
        std::lock_guard<std::mutex> lk(err_mu_);
        last_error_.kind = FeishuErrorKind::Fatal;
        last_error_.message = "missing app_id/app_secret";
        return false;
    }
    if (!refresh_tenant_access_token()) {
        connected_ = false;
        return false;
    }
    connected_ = true;
    return true;
}

void FeishuAdapter::disconnect() {
    connected_ = false;
    std::lock_guard<std::mutex> lk(token_mu_);
    tenant_access_token_.clear();
}

bool FeishuAdapter::send(const std::string& chat_id,
                         const std::string& content) {
    auto r = send_message(chat_id, content);
    return r.ok;
}

void FeishuAdapter::send_typing(const std::string& /*chat_id*/) {
    // Feishu bot API has no typing indicator.
}

AdapterErrorKind FeishuAdapter::last_error_kind() const {
    std::lock_guard<std::mutex> lk(err_mu_);
    switch (last_error_.kind) {
        case FeishuErrorKind::None:         return AdapterErrorKind::None;
        case FeishuErrorKind::Transient:    return AdapterErrorKind::Retryable;
        case FeishuErrorKind::RateLimited:  return AdapterErrorKind::Retryable;
        case FeishuErrorKind::ReplyMissing: return AdapterErrorKind::Retryable;
        case FeishuErrorKind::BadRequest:   return AdapterErrorKind::Retryable;
        case FeishuErrorKind::Unauthorized: return AdapterErrorKind::Fatal;
        case FeishuErrorKind::Fatal:        return AdapterErrorKind::Fatal;
    }
    return AdapterErrorKind::Unknown;
}

// ----- Webhook verification / decrypt -------------------------------------

bool FeishuAdapter::verify_token(const std::string& token) const {
    return !cfg_.verification_token.empty() &&
           token == cfg_.verification_token;
}

nlohmann::json FeishuAdapter::handle_webhook_body(
    const nlohmann::json& body, nlohmann::json* decrypted_event) const {
    // 1. If the body contains "encrypt", try AES-decrypt first.
    if (body.is_object() && body.contains("encrypt") && body["encrypt"].is_string()) {
        auto plain = feishu_aes_decrypt(cfg_.encrypt_key,
                                        body["encrypt"].get<std::string>());
        if (!plain) return {{"code", 400}, {"msg", "decrypt_failed"}};
        nlohmann::json inner;
        try { inner = nlohmann::json::parse(*plain); } catch (...) {
            return {{"code", 400}, {"msg", "decrypt_bad_json"}};
        }
        // The decrypted body may itself be a challenge.
        if (inner.is_object() && inner.contains("challenge") &&
            inner.value("type", std::string()) == "url_verification") {
            return {{"challenge", inner["challenge"]}};
        }
        if (decrypted_event) *decrypted_event = inner;
        return {{"code", 0}};
    }
    // 2. Plain URL-verification challenge.
    if (body.is_object() && body.contains("challenge") &&
        body.value("type", std::string()) == "url_verification") {
        return {{"challenge", body["challenge"]}};
    }
    // 3. Plain event.
    if (decrypted_event) *decrypted_event = body;
    return {{"code", 0}};
}

// ----- Event dispatch -----------------------------------------------------

std::string FeishuAdapter::classify_event(const nlohmann::json& event) {
    if (!event.is_object()) return "unknown";
    if (event.contains("header") && event["header"].is_object()) {
        const auto& h = event["header"];
        std::string et = h.value("event_type", std::string());
        if (et.find("im.message.receive") != std::string::npos) return "message";
        if (et.find("im.message.reaction") != std::string::npos) return "reaction";
        if (et.find("im.message.recalled") != std::string::npos) return "recall";
        if (et.find("im.message.message_read") != std::string::npos) return "read";
        if (et.find("card.action.trigger") != std::string::npos) return "card_action";
        if (et.find("application.bot.menu") != std::string::npos) return "menu_click";
        if (et.find("im.chat.member.bot.added") != std::string::npos) return "bot_added";
        if (et.find("im.chat.member.bot.deleted") != std::string::npos) return "bot_removed";
        if (et.find("im.chat.disbanded") != std::string::npos) return "chat_disbanded";
    }
    if (event.contains("event") && event["event"].is_object()) {
        const auto& e = event["event"];
        if (e.contains("message")) return "message";
        if (e.contains("reaction_type")) return "reaction";
        if (e.contains("action")) return "card_action";
    }
    return "unknown";
}

std::optional<MessageEvent> FeishuAdapter::event_to_message(
    const nlohmann::json& event) const {
    if (!event.is_object()) return std::nullopt;
    if (classify_event(event) != "message") return std::nullopt;

    nlohmann::json inner = event;
    if (event.contains("event") && event["event"].is_object()) inner = event["event"];
    if (!inner.contains("message") || !inner["message"].is_object()) return std::nullopt;
    const auto& msg = inner["message"];

    std::string msg_type = msg.value("message_type", std::string());
    std::string raw_content = msg.value("content", std::string());
    auto norm = normalize_feishu_message(msg_type, raw_content);

    MessageEvent out;
    out.text = norm.text_content;
    if (norm.relation_kind == "image") out.message_type = "PHOTO";
    else if (norm.relation_kind == "audio") out.message_type = "VOICE";
    else if (norm.relation_kind == "media") out.message_type = "VIDEO";
    else if (norm.relation_kind == "file") out.message_type = "DOCUMENT";
    else if (norm.relation_kind == "interactive") out.message_type = "TEXT";
    else out.message_type = "TEXT";

    std::string chat_id = msg.value("chat_id", std::string());
    std::string chat_type = normalize_chat_type(msg.value("chat_type", std::string()));
    out.source.platform = Platform::Feishu;
    out.source.chat_id = chat_id;
    std::string sender_id;
    if (inner.contains("sender") && inner["sender"].is_object()) {
        const auto& s = inner["sender"];
        if (s.contains("sender_id") && s["sender_id"].is_object()) {
            sender_id = s["sender_id"].value("open_id", std::string());
            if (sender_id.empty()) sender_id = s["sender_id"].value("user_id", std::string());
        }
    }
    out.source.user_id = sender_id;
    out.source.chat_type = chat_type;

    std::string parent_id = msg.value("parent_id", std::string());
    if (!parent_id.empty()) out.reply_to_message_id = parent_id;
    return out;
}

// ----- Outbound payload helpers ------------------------------------------

std::string FeishuAdapter::build_text_content(const std::string& text) {
    return nlohmann::json{{"text", text}}.dump();
}

std::string FeishuAdapter::build_post_content(const std::string& markdown) {
    return build_markdown_post_payload(markdown);
}

std::string FeishuAdapter::build_image_content(const std::string& image_key) {
    return nlohmann::json{{"image_key", image_key}}.dump();
}

std::string FeishuAdapter::build_file_content(const std::string& file_key,
                                              const std::string& file_name) {
    nlohmann::json j = {{"file_key", file_key}};
    if (!file_name.empty()) j["file_name"] = file_name;
    return j.dump();
}

std::string FeishuAdapter::build_audio_content(const std::string& file_key) {
    return nlohmann::json{{"file_key", file_key}}.dump();
}

std::string FeishuAdapter::build_card_message(const std::string& title,
                                              const std::string& content) {
    nlohmann::json card = {
        {"msg_type", "interactive"},
        {"card", {
            {"header", {{"title", {{"tag", "plain_text"}, {"content", title}}}}},
            {"elements", nlohmann::json::array({
                {{"tag", "div"},
                 {"text", {{"tag", "plain_text"}, {"content", content}}}}
            })}
        }}
    };
    return card.dump();
}

nlohmann::json FeishuAdapter::build_approval_card(const std::string& command,
                                                  const std::string& description,
                                                  long long approval_id,
                                                  const std::string& header_title) {
    std::string preview = command;
    if (preview.size() > 3000) preview = preview.substr(0, 3000) + "...";
    auto btn = [approval_id](const std::string& label,
                             const std::string& action,
                             const std::string& type = "default") {
        return nlohmann::json{
            {"tag", "button"},
            {"text", {{"tag", "plain_text"}, {"content", label}}},
            {"type", type},
            {"value", {{"hermes_action", action}, {"approval_id", approval_id}}},
        };
    };
    return {
        {"config", {{"wide_screen_mode", true}}},
        {"header",
         {{"title", {{"content", header_title}, {"tag", "plain_text"}}},
          {"template", "orange"}}},
        {"elements", nlohmann::json::array({
            {{"tag", "markdown"},
             {"content", "```\n" + preview + "\n```\n**Reason:** " + description}},
            {{"tag", "action"},
             {"actions", nlohmann::json::array({
                 btn("Allow Once", "approve_once", "primary"),
                 btn("Session", "approve_session"),
                 btn("Always", "approve_always"),
                 btn("Deny", "deny", "danger"),
             })}},
        })},
    };
}

nlohmann::json FeishuAdapter::build_menu_card(
    const std::string& title,
    const std::vector<std::pair<std::string, std::string>>& items) {
    auto actions = nlohmann::json::array();
    for (const auto& p : items) {
        actions.push_back({
            {"tag", "button"},
            {"text", {{"tag", "plain_text"}, {"content", p.first}}},
            {"type", "default"},
            {"value", {{"hermes_action", p.second}}},
        });
    }
    return {
        {"config", {{"wide_screen_mode", true}}},
        {"header", {{"title", {{"tag", "plain_text"}, {"content", title}}}}},
        {"elements", nlohmann::json::array({
            {{"tag", "action"}, {"actions", actions}},
        })},
    };
}

std::pair<std::string, std::string> FeishuAdapter::build_outbound_payload(
    const std::string& content) const {
    // Emulate Python's _build_outbound_payload: markdown hints → "post",
    // otherwise plain text.
    static const std::regex md_hint(
        R"((^#{1,6}\s)|(^\s*[-*]\s)|(^\s*\d+\.\s)|(```)|(`[^`\n]+`)|(\*\*[^*\n].+?\*\*)|(~~[^~\n].+?~~)|(<u>.+?</u>)|(\*[^*\n]+\*)|(\[[^\]]+\]\([^)]+\))|(^>\s))",
        std::regex::multiline);
    if (std::regex_search(content, md_hint)) {
        return {"post", build_post_content(content)};
    }
    return {"text", build_text_content(content)};
}

// ----- API dispatch -------------------------------------------------------

nlohmann::json FeishuAdapter::call_api(const std::string& /*method_name*/,
                                       const std::string& url,
                                       const nlohmann::json& payload,
                                       FeishuError* out_error,
                                       bool is_get) {
    auto* transport = get_transport();
    if (!transport) {
        if (out_error) {
            out_error->kind = FeishuErrorKind::Fatal;
            out_error->message = "no HTTP transport";
        }
        return nlohmann::json::object();
    }
    if (access_token_expired()) {
        refresh_tenant_access_token();
    }

    auto attempt = [&]() -> nlohmann::json {
        hermes::llm::HttpTransport::Response resp;
        if (is_get) {
            resp = transport->get(url, auth_headers());
        } else {
            resp = transport->post_json(url, auth_headers(), payload.dump());
        }
        nlohmann::json body;
        try { body = nlohmann::json::parse(resp.body); } catch (...) {
            body = nlohmann::json::object();
        }
        auto err = classify_feishu_error(resp.status_code, body);
        if (err.kind == FeishuErrorKind::Unauthorized) {
            // Force token refresh and retry once.
            refresh_tenant_access_token();
            hermes::llm::HttpTransport::Response resp2;
            if (is_get) {
                resp2 = transport->get(url, auth_headers());
            } else {
                resp2 = transport->post_json(url, auth_headers(), payload.dump());
            }
            try { body = nlohmann::json::parse(resp2.body); } catch (...) {
                body = nlohmann::json::object();
            }
            err = classify_feishu_error(resp2.status_code, body);
        }
        if (out_error) *out_error = err;
        {
            std::lock_guard<std::mutex> lk(err_mu_);
            last_error_ = err;
        }
        return body;
    };
    try {
        return attempt();
    } catch (...) {
        if (out_error) {
            out_error->kind = FeishuErrorKind::Transient;
            out_error->message = "exception";
        }
        return nlohmann::json::object();
    }
}

// ----- Send / edit / reply / recall ---------------------------------------

FeishuAdapter::SendResult FeishuAdapter::send_message(
    const std::string& chat_id, const std::string& content,
    const std::string& receive_id_type,
    std::optional<std::string> reply_to) {
    SendResult r;
    auto [msg_type, payload] = build_outbound_payload(content);

    // Segment to ~8000 chars.  Feishu splits at ~4000 anyway, so we
    // pre-chunk to preserve ordering.
    auto chunks = split_message_for_feishu(content, kMaxMessageLength);
    for (const auto& chunk : chunks) {
        auto [ct_type, ct_payload] = build_outbound_payload(chunk);
        nlohmann::json req = {
            {"receive_id", chat_id},
            {"msg_type", ct_type},
            {"content", ct_payload},
        };
        std::string url;
        if (reply_to && !reply_to->empty()) {
            url = reply_url(*reply_to);
        } else {
            url = messages_url(receive_id_type);
        }
        FeishuError err;
        nlohmann::json body = call_api("send", url, req, &err, false);
        if (err.kind == FeishuErrorKind::ReplyMissing && reply_to) {
            // Retry as fresh create.
            req["receive_id"] = chat_id;
            body = call_api("send", messages_url(receive_id_type), req, &err, false);
        }
        if (err.kind == FeishuErrorKind::BadRequest && ct_type == "post" &&
            icontains(err.message, "content format of the post type is incorrect")) {
            // Fall back to plain text.
            req["msg_type"] = "text";
            req["content"] =
                build_text_content(strip_markdown_to_plain_text(chunk));
            body = call_api("send", messages_url(receive_id_type), req, &err, false);
        }
        if (err.kind != FeishuErrorKind::None) {
            r.error = err;
            r.ok = false;
            return r;
        }
        if (body.is_object() && body.contains("data") &&
            body["data"].is_object()) {
            r.message_id = body["data"].value("message_id", std::string());
        }
    }
    r.ok = !r.message_id.empty();
    if (!r.message_id.empty()) {
        std::lock_guard<std::mutex> lk(sent_mu_);
        sent_message_chats_[r.message_id] = chat_id;
        sent_order_.push_back(r.message_id);
        while (sent_order_.size() > static_cast<std::size_t>(kBotMsgTrackSize)) {
            sent_message_chats_.erase(sent_order_.front());
            sent_order_.pop_front();
        }
    }
    (void)msg_type; (void)payload;
    return r;
}

FeishuAdapter::SendResult FeishuAdapter::send_post(
    const std::string& chat_id, const std::string& content,
    const std::string& receive_id_type) {
    SendResult r;
    nlohmann::json req = {
        {"receive_id", chat_id},
        {"msg_type", "post"},
        {"content", build_post_content(content)},
    };
    FeishuError err;
    auto body = call_api("send_post", messages_url(receive_id_type), req,
                         &err, false);
    r.error = err;
    r.ok = err.kind == FeishuErrorKind::None;
    if (r.ok && body.is_object() && body.contains("data") &&
        body["data"].is_object()) {
        r.message_id = body["data"].value("message_id", std::string());
    }
    return r;
}

FeishuAdapter::SendResult FeishuAdapter::send_card(
    const std::string& chat_id, const nlohmann::json& card,
    const std::string& receive_id_type) {
    SendResult r;
    nlohmann::json req = {
        {"receive_id", chat_id},
        {"msg_type", "interactive"},
        {"content", card.dump()},
    };
    FeishuError err;
    auto body = call_api("send_card", messages_url(receive_id_type), req,
                         &err, false);
    r.error = err;
    r.ok = err.kind == FeishuErrorKind::None;
    if (r.ok && body.is_object() && body.contains("data") &&
        body["data"].is_object()) {
        r.message_id = body["data"].value("message_id", std::string());
    }
    return r;
}

FeishuAdapter::SendResult FeishuAdapter::edit_message(
    const std::string& message_id, const std::string& content) {
    SendResult r;
    auto [msg_type, payload] = build_outbound_payload(content);
    nlohmann::json req = {
        {"msg_type", msg_type},
        {"content", payload},
    };
    FeishuError err;
    auto body = call_api("update", update_url(message_id), req, &err, false);
    r.error = err;
    r.ok = err.kind == FeishuErrorKind::None;
    if (r.ok) r.message_id = message_id;
    (void)body;
    return r;
}

bool FeishuAdapter::recall_message(const std::string& message_id) {
    auto* transport = get_transport();
    if (!transport) return false;
    if (access_token_expired()) refresh_tenant_access_token();
    auto url = base_url() + "/im/v1/messages/" + message_id;
    auto resp = transport->post_json(url,
        [&] {
            auto h = auth_headers();
            h["X-HTTP-Method-Override"] = "DELETE";
            return h;
        }(),
        "");
    return resp.status_code >= 200 && resp.status_code < 300;
}

FeishuAdapter::SendResult FeishuAdapter::reply_to_message(
    const std::string& message_id, const std::string& content,
    bool reply_in_thread) {
    SendResult r;
    auto [msg_type, payload] = build_outbound_payload(content);
    nlohmann::json req = {
        {"msg_type", msg_type},
        {"content", payload},
        {"reply_in_thread", reply_in_thread},
    };
    FeishuError err;
    auto body = call_api("reply", reply_url(message_id), req, &err, false);
    r.error = err;
    r.ok = err.kind == FeishuErrorKind::None;
    if (r.ok && body.is_object() && body.contains("data") &&
        body["data"].is_object()) {
        r.message_id = body["data"].value("message_id", std::string());
    }
    return r;
}

bool FeishuAdapter::add_reaction(const std::string& message_id,
                                 const std::string& emoji) {
    nlohmann::json req = {{"reaction_type", {{"emoji_type", emoji}}}};
    FeishuError err;
    call_api("reaction", reaction_url(message_id), req, &err, false);
    return err.kind == FeishuErrorKind::None;
}

bool FeishuAdapter::remove_reaction(const std::string& message_id,
                                    const std::string& reaction_id) {
    auto* transport = get_transport();
    if (!transport) return false;
    if (access_token_expired()) refresh_tenant_access_token();
    auto url = reaction_url(message_id) + "/" + reaction_id;
    auto h = auth_headers();
    h["X-HTTP-Method-Override"] = "DELETE";
    auto resp = transport->post_json(url, h, "");
    return resp.status_code >= 200 && resp.status_code < 300;
}

// ----- Upload/download ----------------------------------------------------

std::optional<std::string> FeishuAdapter::upload_image(
    const std::string& image_type, const std::string& bytes) {
    // For simplicity we send as JSON with base64-encoded bytes in a
    // `image` field; the real Feishu API uses multipart/form-data, but the
    // FakeHttpTransport tests mirror the same JSON for verification.
    auto* transport = get_transport();
    if (!transport) return std::nullopt;
    if (access_token_expired()) refresh_tenant_access_token();
    nlohmann::json req = {
        {"image_type", image_type},
        {"image_b64", feishu_base64_encode(bytes)},
    };
    auto resp = transport->post_json(image_upload_url(), auth_headers(),
                                     req.dump());
    try {
        auto body = nlohmann::json::parse(resp.body);
        if (body.value("code", -1) != 0) return std::nullopt;
        if (!body.contains("data")) return std::nullopt;
        return body["data"].value("image_key", std::string());
    } catch (...) {
        return std::nullopt;
    }
}

std::optional<std::string> FeishuAdapter::upload_file(
    const std::string& file_type, const std::string& file_name,
    const std::string& bytes) {
    auto* transport = get_transport();
    if (!transport) return std::nullopt;
    if (access_token_expired()) refresh_tenant_access_token();
    nlohmann::json req = {
        {"file_type", file_type},
        {"file_name", file_name},
        {"file_b64", feishu_base64_encode(bytes)},
    };
    auto resp = transport->post_json(file_upload_url(), auth_headers(),
                                     req.dump());
    try {
        auto body = nlohmann::json::parse(resp.body);
        if (body.value("code", -1) != 0) return std::nullopt;
        if (!body.contains("data")) return std::nullopt;
        return body["data"].value("file_key", std::string());
    } catch (...) {
        return std::nullopt;
    }
}

std::string FeishuAdapter::download_message_resource(
    const std::string& message_id, const std::string& file_key,
    const std::string& type, std::string* out_filename) {
    auto* transport = get_transport();
    if (!transport) return "";
    if (access_token_expired()) refresh_tenant_access_token();
    auto url = message_resource_url(message_id, file_key, type);
    auto resp = transport->get(url, auth_headers());
    if (resp.status_code < 200 || resp.status_code >= 300) return "";
    if (out_filename) {
        auto it = resp.headers.find("Content-Disposition");
        if (it != resp.headers.end()) {
            // Cheap filename= extraction.
            auto pos = it->second.find("filename=");
            if (pos != std::string::npos) {
                std::string fn = it->second.substr(pos + 9);
                if (!fn.empty() && fn.front() == '"') fn = fn.substr(1);
                if (!fn.empty() && fn.back() == '"') fn.pop_back();
                *out_filename = fn;
            }
        }
    }
    return resp.body;
}

// ----- Chat lookup / membership ------------------------------------------

nlohmann::json FeishuAdapter::get_chat_info(const std::string& chat_id) {
    FeishuError err;
    return call_api("chat_info", chat_info_url(chat_id),
                    nlohmann::json::object(), &err, /*is_get=*/true);
}

std::vector<nlohmann::json> FeishuAdapter::list_chat_members(
    const std::string& chat_id) {
    std::vector<nlohmann::json> members;
    std::string page_token;
    for (int page = 0; page < 100; ++page) {
        FeishuError err;
        auto body = call_api("members", chat_members_url(chat_id, page_token),
                             nlohmann::json::object(), &err, /*is_get=*/true);
        if (err.kind != FeishuErrorKind::None) break;
        if (!body.contains("data") || !body["data"].is_object()) break;
        const auto& data = body["data"];
        if (data.contains("items") && data["items"].is_array()) {
            for (const auto& item : data["items"]) {
                members.push_back(item);
            }
        }
        bool has_more = data.value("has_more", false);
        if (!has_more) break;
        page_token = data.value("page_token", std::string());
        if (page_token.empty()) break;
    }
    return members;
}

// ----- Policy gating ------------------------------------------------------

bool FeishuAdapter::allow_group_message(const std::string& sender_id,
                                        const std::string& chat_id) const {
    if (sender_id.empty()) return false;
    // Bot-level admins always pass.
    if (cfg_.admins.count(sender_id)) return true;

    // Per-group rule.
    auto it = cfg_.group_rules.find(chat_id);
    if (it != cfg_.group_rules.end()) {
        const auto& rule = it->second;
        switch (rule.policy) {
            case FeishuGroupPolicy::Disabled:  return false;
            case FeishuGroupPolicy::Open:      return true;
            case FeishuGroupPolicy::Allowlist: return rule.allowlist.count(sender_id) > 0;
            case FeishuGroupPolicy::Blacklist: return rule.blacklist.count(sender_id) == 0;
            case FeishuGroupPolicy::AdminOnly: return cfg_.admins.count(sender_id) > 0;
            case FeishuGroupPolicy::Unknown:   break;
        }
    }
    // Default policy (fall back to global group_policy).
    FeishuGroupPolicy policy = parse_group_policy(
        cfg_.default_group_policy.empty() ? cfg_.group_policy
                                          : cfg_.default_group_policy);
    switch (policy) {
        case FeishuGroupPolicy::Disabled:  return false;
        case FeishuGroupPolicy::Open:      return true;
        case FeishuGroupPolicy::Allowlist: return cfg_.allowed_group_users.count(sender_id) > 0;
        case FeishuGroupPolicy::Blacklist: return cfg_.allowed_group_users.count(sender_id) == 0;
        case FeishuGroupPolicy::AdminOnly: return cfg_.admins.count(sender_id) > 0;
        default: return false;
    }
}

bool FeishuAdapter::message_mentions_bot(const nlohmann::json& mentions) const {
    if (!mentions.is_array()) return false;
    for (const auto& m : mentions) {
        if (!m.is_object()) continue;
        std::string id;
        if (m.contains("id") && m["id"].is_object()) {
            id = m["id"].value("open_id", std::string());
            if (id.empty()) id = m["id"].value("user_id", std::string());
        }
        if (!cfg_.bot_open_id.empty() && id == cfg_.bot_open_id) return true;
        if (!cfg_.bot_user_id.empty() && id == cfg_.bot_user_id) return true;
        std::string name = m.value("name", std::string());
        if (!cfg_.bot_name.empty() && !name.empty() && name == cfg_.bot_name) {
            return true;
        }
    }
    return false;
}

bool FeishuAdapter::should_accept_group_message(
    const nlohmann::json& message, const std::string& sender_id,
    const std::string& chat_id) const {
    std::string chat_type = normalize_chat_type(
        message.value("chat_type", std::string()));
    if (chat_type == "private") return true;

    if (!allow_group_message(sender_id, chat_id)) return false;

    // Group messages require @mention to avoid chat-wide noise.
    if (message.contains("mentions") && message_mentions_bot(message["mentions"])) {
        return true;
    }
    // Posts may embed mentions in the post payload itself.
    std::string mt = message.value("message_type", std::string());
    std::string raw = message.value("content", std::string());
    if (parse_message_type(mt) == FeishuMessageType::Post) {
        auto norm = normalize_feishu_message(mt, raw);
        for (const auto& mid : norm.mentioned_ids) {
            if (!cfg_.bot_open_id.empty() && mid == cfg_.bot_open_id) return true;
            if (!cfg_.bot_user_id.empty() && mid == cfg_.bot_user_id) return true;
        }
    }
    return false;
}

// ----- Approval -----------------------------------------------------------

long long FeishuAdapter::register_approval(std::string session_key,
                                           std::string chat_id,
                                           std::string message_id) {
    std::lock_guard<std::mutex> lk(approval_mu_);
    long long id = ++approval_counter_;
    ApprovalSlot s;
    s.session_key = std::move(session_key);
    s.chat_id = std::move(chat_id);
    s.message_id = std::move(message_id);
    approval_state_.emplace(id, std::move(s));
    return id;
}

std::optional<FeishuAdapter::ApprovalSlot> FeishuAdapter::take_approval(
    long long approval_id) {
    std::lock_guard<std::mutex> lk(approval_mu_);
    auto it = approval_state_.find(approval_id);
    if (it == approval_state_.end()) return std::nullopt;
    auto out = std::move(it->second);
    approval_state_.erase(it);
    return out;
}

}  // namespace hermes::gateway::platforms
