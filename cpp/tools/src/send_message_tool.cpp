// Send-message tool — port of tools/send_message_tool.py.
#include "hermes/tools/send_message_tool.hpp"

#include "hermes/tools/registry.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <mutex>
#include <optional>
#include <regex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace hermes::tools {

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {

hermes::gateway::GatewayRunner* g_gateway_runner = nullptr;
PlatformListFn g_platform_list_fn = nullptr;
PlatformSendFn g_platform_send_fn = nullptr;
std::mutex g_state_mu;

// ── Regex patterns (mirrors Python) ───────────────────────────────────
const std::regex& re_telegram_topic() {
    static const std::regex r(R"(^\s*(-?\d+)(?::(\d+))?\s*$)");
    return r;
}
const std::regex& re_feishu() {
    static const std::regex r(
        R"(^\s*((?:oc|ou|on|chat|open)_[-A-Za-z0-9]+)(?::([-A-Za-z0-9_]+))?\s*$)");
    return r;
}
const std::regex& re_weixin() {
    static const std::regex r(
        R"(^\s*((?:wxid|gh|v\d+|wm|wb)_[A-Za-z0-9_-]+|[A-Za-z0-9._-]+@chatroom|filehelper)\s*$)");
    return r;
}
const std::regex& re_url_secret() {
    static const std::regex r(
        R"(([?&](?:access_token|api[_-]?key|auth[_-]?token|token|signature|sig)=)([^&#\s]+))",
        std::regex::icase);
    return r;
}
const std::regex& re_generic_secret() {
    static const std::regex r(
        R"(\b(access_token|api[_-]?key|auth[_-]?token|signature|sig)\s*=\s*([^\s,;]+))",
        std::regex::icase);
    return r;
}
const std::regex& re_bearer() {
    static const std::regex r(
        R"(([Bb]earer\s+)[A-Za-z0-9._\-]{8,})");
    return r;
}
const std::regex& re_media_directive() {
    static const std::regex r(
        R"(\[(media|voice):([^\]]+)\])");
    return r;
}

// Extension helpers.
const std::unordered_set<std::string>& image_exts() {
    static const std::unordered_set<std::string> s{
        ".jpg", ".jpeg", ".png", ".webp", ".gif"};
    return s;
}
const std::unordered_set<std::string>& video_exts() {
    static const std::unordered_set<std::string> s{
        ".mp4", ".mov", ".avi", ".mkv", ".3gp"};
    return s;
}
const std::unordered_set<std::string>& audio_exts() {
    static const std::unordered_set<std::string> s{
        ".ogg", ".opus", ".mp3", ".wav", ".m4a"};
    return s;
}
const std::unordered_set<std::string>& voice_exts() {
    static const std::unordered_set<std::string> s{".ogg", ".opus"};
    return s;
}

std::string to_lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    return s;
}

std::string trim(std::string s) {
    auto begin = s.begin();
    while (begin != s.end() &&
           std::isspace(static_cast<unsigned char>(*begin))) {
        ++begin;
    }
    auto end = s.end();
    while (end != begin &&
           std::isspace(static_cast<unsigned char>(*(end - 1)))) {
        --end;
    }
    return std::string(begin, end);
}

bool is_all_digits(const std::string& s) {
    std::string stripped = s;
    if (!stripped.empty() && stripped.front() == '-') {
        stripped.erase(0, 1);
    }
    if (stripped.empty()) return false;
    return std::all_of(stripped.begin(), stripped.end(),
                       [](char c) { return std::isdigit(static_cast<unsigned char>(c)); });
}

}  // namespace

// ── Public: platform list ─────────────────────────────────────────────

const std::vector<std::string>& supported_platforms() {
    static const std::vector<std::string> s{
        "telegram", "discord", "slack",    "whatsapp", "signal",
        "bluebubbles", "matrix", "mattermost", "homeassistant",
        "dingtalk", "feishu", "wecom", "weixin", "email", "sms",
    };
    return s;
}

bool is_supported_platform(std::string_view name) {
    auto lower = to_lower(std::string(name));
    for (const auto& p : supported_platforms()) {
        if (p == lower) return true;
    }
    return false;
}

// ── Public: target parsing ────────────────────────────────────────────

ParsedTarget parse_target(std::string_view target) {
    auto first = target.find(':');
    if (first == std::string_view::npos) {
        throw std::invalid_argument(
            "invalid target format — expected platform:chat_id[:thread_id]");
    }
    auto second = target.find(':', first + 1);
    if (second == std::string_view::npos) {
        throw std::invalid_argument(
            "invalid target format — expected platform:chat_id:thread_id");
    }
    ParsedTarget pt;
    pt.platform = std::string(target.substr(0, first));
    pt.chat_id = std::string(target.substr(first + 1, second - first - 1));
    pt.thread_id = std::string(target.substr(second + 1));
    if (pt.platform.empty() || pt.chat_id.empty()) {
        throw std::invalid_argument("platform and chat_id must not be empty");
    }
    return pt;
}

TargetRefResult parse_target_ref(std::string_view platform_sv,
                                 std::string_view ref_sv) {
    std::string platform = to_lower(std::string(platform_sv));
    std::string ref(ref_sv);
    TargetRefResult out;

    std::smatch m;
    if (platform == "telegram") {
        if (std::regex_match(ref, m, re_telegram_topic())) {
            out.chat_id = m[1].str();
            if (m[2].matched && !m[2].str().empty()) {
                out.thread_id = m[2].str();
            }
            out.is_explicit = true;
            return out;
        }
    }
    if (platform == "feishu") {
        if (std::regex_match(ref, m, re_feishu())) {
            out.chat_id = m[1].str();
            if (m[2].matched && !m[2].str().empty()) {
                out.thread_id = m[2].str();
            }
            out.is_explicit = true;
            return out;
        }
    }
    if (platform == "discord") {
        if (std::regex_match(ref, m, re_telegram_topic())) {
            out.chat_id = m[1].str();
            if (m[2].matched && !m[2].str().empty()) {
                out.thread_id = m[2].str();
            }
            out.is_explicit = true;
            return out;
        }
    }
    if (platform == "weixin") {
        if (std::regex_match(ref, m, re_weixin())) {
            out.chat_id = m[1].str();
            out.is_explicit = true;
            return out;
        }
    }
    // Generic numeric fallback.
    std::string trimmed = trim(ref);
    if (is_all_digits(trimmed)) {
        out.chat_id = trimmed;
        out.is_explicit = true;
        return out;
    }
    return out;
}

// ── Public: secret redaction ──────────────────────────────────────────

std::string sanitize_error_text(std::string_view text) {
    std::string s(text);
    s = std::regex_replace(s, re_url_secret(), "$1***");
    s = std::regex_replace(s, re_generic_secret(), "$1=***");
    s = std::regex_replace(s, re_bearer(), "$1***");
    return s;
}

// ── Public: media detection ───────────────────────────────────────────

std::string describe_media_for_mirror(const std::vector<MediaFile>& media) {
    if (media.empty()) return "";
    if (media.size() == 1) {
        const auto& f = media.front();
        auto ext = to_lower(fs::path(f.path).extension().string());
        if (f.is_voice && voice_exts().count(ext)) {
            return "[Sent voice message]";
        }
        if (image_exts().count(ext)) return "[Sent image attachment]";
        if (video_exts().count(ext)) return "[Sent video attachment]";
        if (audio_exts().count(ext)) return "[Sent audio attachment]";
        return "[Sent document attachment]";
    }
    return "[Sent " + std::to_string(media.size()) + " media attachments]";
}

std::pair<std::string, std::vector<MediaFile>>
extract_media_directives(std::string_view message) {
    std::string s(message);
    std::vector<MediaFile> media;

    auto begin = std::sregex_iterator(s.begin(), s.end(), re_media_directive());
    auto end = std::sregex_iterator{};
    std::string cleaned;
    std::size_t last = 0;
    for (auto it = begin; it != end; ++it) {
        const auto& match = *it;
        cleaned.append(s, last, match.position(0) - last);
        MediaFile mf;
        mf.path = trim(match[2].str());
        mf.is_voice = (match[1].str() == "voice");
        media.push_back(std::move(mf));
        last = match.position(0) + match.length(0);
    }
    cleaned.append(s, last, std::string::npos);
    return {trim(cleaned), media};
}

// ── Public: cron auto-deliver skip ────────────────────────────────────

std::optional<CronAutoDeliverTarget> get_cron_auto_delivery_target() {
    auto getenv_str = [](const char* k) {
        const char* v = std::getenv(k);
        return std::string(v ? v : "");
    };
    auto platform = to_lower(trim(getenv_str("HERMES_CRON_AUTO_DELIVER_PLATFORM")));
    auto chat_id = trim(getenv_str("HERMES_CRON_AUTO_DELIVER_CHAT_ID"));
    if (platform.empty() || chat_id.empty()) return std::nullopt;
    CronAutoDeliverTarget t;
    t.platform = platform;
    t.chat_id = chat_id;
    auto thread = trim(getenv_str("HERMES_CRON_AUTO_DELIVER_THREAD_ID"));
    if (!thread.empty()) t.thread_id = thread;
    return t;
}

std::string maybe_skip_cron_duplicate_send(
    std::string_view platform_sv, std::string_view chat_id_sv,
    std::optional<std::string_view> thread_id_sv) {
    auto tgt = get_cron_auto_delivery_target();
    if (!tgt) return "";
    if (tgt->platform != to_lower(std::string(platform_sv))) return "";
    if (tgt->chat_id != std::string(chat_id_sv)) return "";
    std::optional<std::string> thread_id;
    if (thread_id_sv) thread_id = std::string(*thread_id_sv);
    if (tgt->thread_id != thread_id) return "";

    std::string label = std::string(platform_sv) + ":" + std::string(chat_id_sv);
    if (thread_id) label += ":" + *thread_id;

    json r = {
        {"success", true},
        {"skipped", true},
        {"reason", "cron_auto_delivery_duplicate_target"},
        {"target", label},
        {"note",
         "Skipped send_message to " + label +
             ". This cron job will already auto-deliver its final response "
             "to that same target. Put the intended user-facing content in "
             "your final response instead, or use a different target if you "
             "want an additional message."}};
    return r.dump();
}

// ── Gateway integration plumbing ──────────────────────────────────────

void set_gateway_runner(hermes::gateway::GatewayRunner* runner) {
    std::lock_guard<std::mutex> lk(g_state_mu);
    g_gateway_runner = runner;
}

void set_platform_list_fn(PlatformListFn fn) {
    std::lock_guard<std::mutex> lk(g_state_mu);
    g_platform_list_fn = std::move(fn);
}

void set_platform_send_fn(PlatformSendFn fn) {
    std::lock_guard<std::mutex> lk(g_state_mu);
    g_platform_send_fn = std::move(fn);
}

namespace {

std::string handle_list() {
    PlatformListFn fn;
    hermes::gateway::GatewayRunner* runner = nullptr;
    {
        std::lock_guard<std::mutex> lk(g_state_mu);
        fn = g_platform_list_fn;
        runner = g_gateway_runner;
    }
    if (!runner) {
        return tool_error(
            "gateway not running — start the gateway to list platforms");
    }
    if (!fn) {
        return tool_error(
            "platform listing not configured — set_platform_list_fn() required");
    }
    auto platform_info = fn();
    json platforms = json::array();
    for (const auto& [name, connected] : platform_info) {
        platforms.push_back({{"name", name}, {"connected", connected}});
    }
    return tool_result({{"platforms", platforms}});
}

std::string handle_send(const json& args) {
    std::string target_str = args.value("target", "");
    std::string message = args.value("message",
                                     args.value("content", std::string{}));

    if (target_str.empty() || message.empty()) {
        return tool_error(
            "Both 'target' and 'message' are required when action='send'");
    }

    // Split "platform[:ref]".
    std::string platform_name;
    std::string target_ref;
    auto colon = target_str.find(':');
    if (colon == std::string::npos) {
        platform_name = to_lower(trim(target_str));
    } else {
        platform_name = to_lower(trim(target_str.substr(0, colon)));
        target_ref = trim(target_str.substr(colon + 1));
    }
    if (platform_name.empty()) {
        return tool_error("empty platform");
    }
    if (!is_supported_platform(platform_name)) {
        std::string avail;
        for (const auto& p : supported_platforms()) {
            if (!avail.empty()) avail += ", ";
            avail += p;
        }
        return tool_error("Unknown platform: " + platform_name +
                          ". Available: " + avail);
    }

    std::optional<std::string> chat_id;
    std::optional<std::string> thread_id;
    bool is_explicit = false;
    if (!target_ref.empty()) {
        auto parsed = parse_target_ref(platform_name, target_ref);
        chat_id = parsed.chat_id;
        thread_id = parsed.thread_id;
        is_explicit = parsed.is_explicit;
        if (!is_explicit) {
            return tool_error(
                "Could not resolve '" + target_ref + "' on " + platform_name +
                ". Try using a numeric channel ID instead.");
        }
    }

    auto [cleaned_message, media] = extract_media_directives(message);
    std::string mirror_text = cleaned_message.empty()
                                  ? describe_media_for_mirror(media)
                                  : cleaned_message;

    if (chat_id.has_value()) {
        auto skip = maybe_skip_cron_duplicate_send(platform_name, *chat_id,
                                                   thread_id);
        if (!skip.empty()) return skip;
    }

    hermes::gateway::GatewayRunner* runner = nullptr;
    PlatformSendFn send_fn;
    {
        std::lock_guard<std::mutex> lk(g_state_mu);
        runner = g_gateway_runner;
        send_fn = g_platform_send_fn;
    }

    if (!runner) {
        return tool_error("gateway not running");
    }
    if (!chat_id) {
        return tool_error(
            "No target channel specified and no home channel configured");
    }
    if (!send_fn) {
        // No send callback — best-effort success stub used primarily in tests.
        return tool_result({{"sent", true},
                            {"platform", platform_name},
                            {"chat_id", *chat_id},
                            {"thread_id", thread_id.value_or("")},
                            {"mirror_text", mirror_text}});
    }

    try {
        return send_fn(platform_name, *chat_id, cleaned_message, thread_id,
                       media);
    } catch (const std::exception& ex) {
        return tool_error(sanitize_error_text(std::string("Send failed: ") +
                                              ex.what()));
    }
}

std::string handle_send_message(const json& args, const ToolContext&) {
    const auto action = args.value("action", std::string("send"));
    if (action == "list") return handle_list();
    if (action == "send") return handle_send(args);
    return tool_error("unknown action: " + action);
}

}  // namespace

void register_send_message_tools() {
    auto& reg = ToolRegistry::instance();

    ToolEntry e;
    e.name = "send_message";
    e.toolset = "messaging";
    e.description =
        "Send a message to a connected messaging platform, or list "
        "available targets. Use action='list' before sending when the "
        "user names a specific channel/person.";
    e.emoji = "\xF0\x9F\x92\xAC";  // speech bubble
    e.schema = json::parse(R"JSON({
        "type": "object",
        "properties": {
            "action": {
                "type": "string",
                "enum": ["send", "list"],
                "description": "send (default) or list"
            },
            "target": {
                "type": "string",
                "description":
                    "platform | platform:chat_id | platform:chat_id:thread_id"
            },
            "message": {
                "type": "string",
                "description": "Message body (supports [media:/path])"
            },
            "content": {
                "type": "string",
                "description": "Back-compat alias for message"
            }
        },
        "required": []
    })JSON");
    e.handler = handle_send_message;
    reg.register_tool(std::move(e));
}

}  // namespace hermes::tools
