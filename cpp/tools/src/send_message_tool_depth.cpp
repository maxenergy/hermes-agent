// Implementation of hermes/tools/send_message_tool_depth.hpp.
#include "hermes/tools/send_message_tool_depth.hpp"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <regex>
#include <sstream>
#include <string>

namespace hermes::tools::send_message::depth {

namespace {

std::string to_lower(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        out.push_back(static_cast<char>(
            std::tolower(static_cast<unsigned char>(c))));
    }
    return out;
}

std::string trim_ascii(std::string_view s) {
    std::size_t b{0};
    std::size_t e{s.size()};
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) {
        ++b;
    }
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) {
        --e;
    }
    return std::string{s.substr(b, e - b)};
}

std::string extension_lower(std::string_view path) {
    auto dot = path.rfind('.');
    if (dot == std::string_view::npos) {
        return {};
    }
    // Dot must be in the basename, not a directory part.
    auto slash = path.find_last_of("/\\");
    if (slash != std::string_view::npos && dot < slash) {
        return {};
    }
    return to_lower(path.substr(dot));
}

}  // namespace

// ---- Action routing -----------------------------------------------------

Action parse_action(std::string_view raw) {
    std::string trimmed{to_lower(trim_ascii(raw))};
    if (trimmed.empty()) {
        return Action::Send;
    }
    if (trimmed == "send") {
        return Action::Send;
    }
    if (trimmed == "list") {
        return Action::List;
    }
    return Action::Unknown;
}

std::string action_name(Action action) {
    switch (action) {
        case Action::Send:
            return "send";
        case Action::List:
            return "list";
        case Action::Unknown:
        default:
            return "unknown";
    }
}

// ---- Platform normalisation --------------------------------------------

const std::vector<std::string>& known_platforms() {
    static const std::vector<std::string> k{
        "telegram", "discord", "slack",  "signal",
        "matrix",   "feishu",  "weixin", "whatsapp",
    };
    return k;
}

std::string normalise_platform_name(std::string_view raw) {
    std::string t{to_lower(trim_ascii(raw))};
    for (const auto& p : known_platforms()) {
        if (t == p) {
            return t;
        }
    }
    return {};
}

bool is_known_platform(std::string_view name) {
    for (const auto& p : known_platforms()) {
        if (name == p) {
            return true;
        }
    }
    return false;
}

// ---- Target parsing (wire-level) ---------------------------------------

TargetSpec split_target(std::string_view raw) {
    TargetSpec out{};
    std::string trimmed{trim_ascii(raw)};
    auto colon = trimmed.find(':');
    if (colon == std::string::npos) {
        out.platform = normalise_platform_name(trimmed);
        out.has_colon = false;
        return out;
    }
    out.has_colon = true;
    out.platform = normalise_platform_name(trimmed.substr(0, colon));
    out.remainder = trimmed.substr(colon + 1);
    return out;
}

std::string format_target_label(std::string_view platform,
                                std::string_view chat_id,
                                std::optional<std::string_view> thread_id) {
    std::string out;
    out.reserve(platform.size() + chat_id.size() + 4);
    out.append(platform).push_back(':');
    out.append(chat_id);
    if (thread_id.has_value() && !thread_id->empty()) {
        out.push_back(':');
        out.append(*thread_id);
    }
    return out;
}

// ---- Media classification ----------------------------------------------

const std::unordered_set<std::string>& image_extensions() {
    static const std::unordered_set<std::string> k{
        ".jpg", ".jpeg", ".png", ".webp", ".gif",
    };
    return k;
}

const std::unordered_set<std::string>& video_extensions() {
    static const std::unordered_set<std::string> k{
        ".mp4", ".mov", ".avi", ".mkv", ".3gp",
    };
    return k;
}

const std::unordered_set<std::string>& audio_extensions() {
    static const std::unordered_set<std::string> k{
        ".ogg", ".opus", ".mp3", ".wav", ".m4a",
    };
    return k;
}

const std::unordered_set<std::string>& voice_extensions() {
    static const std::unordered_set<std::string> k{
        ".ogg", ".opus",
    };
    return k;
}

MediaKind classify_media(std::string_view path, bool is_voice) {
    std::string ext{extension_lower(path)};
    if (is_voice && voice_extensions().count(ext) != 0u) {
        return MediaKind::Voice;
    }
    if (image_extensions().count(ext) != 0u) {
        return MediaKind::Image;
    }
    if (video_extensions().count(ext) != 0u) {
        return MediaKind::Video;
    }
    if (audio_extensions().count(ext) != 0u) {
        return MediaKind::Audio;
    }
    return MediaKind::Document;
}

std::string media_kind_label(MediaKind kind) {
    switch (kind) {
        case MediaKind::Image:
            return "[Sent image attachment]";
        case MediaKind::Video:
            return "[Sent video attachment]";
        case MediaKind::Audio:
            return "[Sent audio attachment]";
        case MediaKind::Voice:
            return "[Sent voice message]";
        case MediaKind::Document:
            return "[Sent document attachment]";
        case MediaKind::Unknown:
        default:
            return "[Sent attachment]";
    }
}

// ---- Secret scrubbing ---------------------------------------------------

std::string scrub_url_query_secrets(std::string_view text) {
    static const std::regex re(
        R"(([?&](?:access_token|api[_-]?key|auth[_-]?token|token|signature|sig)=)([^&#\s]+))",
        std::regex::icase);
    return std::regex_replace(std::string{text}, re, "$1***");
}

std::string scrub_generic_secret_assignments(std::string_view text) {
    static const std::regex re(
        R"(\b(access_token|api[_-]?key|auth[_-]?token|signature|sig)\s*=\s*([^\s,;]+))",
        std::regex::icase);
    return std::regex_replace(std::string{text}, re, "$1=***");
}

std::string scrub_secret_patterns(std::string_view text) {
    std::string out{scrub_url_query_secrets(text)};
    return scrub_generic_secret_assignments(out);
}

// ---- Send-response shapes ----------------------------------------------

nlohmann::json missing_target_response() {
    nlohmann::json out;
    out["error"] =
        "No target specified. Use target='<platform>' or "
        "target='<platform>:<chat_id>' (e.g. 'telegram', 'discord:#bot-home').";
    return out;
}

nlohmann::json missing_message_response() {
    nlohmann::json out;
    out["error"] = "No message text provided.";
    return out;
}

nlohmann::json unknown_platform_response(std::string_view platform) {
    nlohmann::json out;
    std::ostringstream os;
    os << "Unknown platform '" << platform << "'. Supported: ";
    bool first{true};
    for (const auto& p : known_platforms()) {
        if (!first) {
            os << ", ";
        }
        first = false;
        os << p;
    }
    out["error"] = os.str();
    return out;
}

nlohmann::json success_response(std::string_view platform,
                                std::string_view chat_id,
                                std::optional<std::string_view> thread_id,
                                std::string_view message) {
    nlohmann::json out;
    out["success"] = true;
    out["platform"] = std::string{platform};
    out["chat_id"] = std::string{chat_id};
    if (thread_id.has_value() && !thread_id->empty()) {
        out["thread_id"] = std::string{*thread_id};
    }
    out["message"] = std::string{message};
    out["target"] = format_target_label(platform, chat_id, thread_id);
    return out;
}

nlohmann::json cron_duplicate_skip_response(std::string_view platform,
                                            std::string_view chat_id,
                                            std::optional<std::string_view> thread_id) {
    nlohmann::json out;
    out["success"] = true;
    out["skipped"] = true;
    out["reason"] = "cron_auto_delivery_duplicate_target";
    out["target"] = format_target_label(platform, chat_id, thread_id);
    out["note"] =
        "Skipped — the cron scheduler will auto-deliver this output to the "
        "same target.";
    return out;
}

// ---- Channel-directory helpers ------------------------------------------

std::vector<std::string> cap_channel_list(const std::vector<std::string>& names,
                                          std::size_t max_entries) {
    if (names.size() <= max_entries || max_entries == 0u) {
        return names;
    }
    std::vector<std::string> out{};
    out.reserve(max_entries + 1u);
    for (std::size_t i{0}; i < max_entries; ++i) {
        out.push_back(names[i]);
    }
    std::ostringstream os;
    os << "… " << (names.size() - max_entries) << " more";
    out.push_back(os.str());
    return out;
}

std::string render_channel_list(const std::vector<std::string>& names) {
    if (names.empty()) {
        return {};
    }
    std::ostringstream os;
    bool first{true};
    for (const auto& n : names) {
        if (!first) {
            os << '\n';
        }
        first = false;
        os << n;
    }
    return os.str();
}

// ---- Telegram topic parsing --------------------------------------------

TelegramTopic parse_telegram_topic(std::string_view ref) {
    TelegramTopic out{};
    static const std::regex re(R"(^\s*(-?\d+)(?::(\d+))?\s*$)");
    std::match_results<std::string_view::const_iterator> m;
    if (!std::regex_match(ref.begin(), ref.end(), m, re)) {
        return out;
    }
    out.matched = true;
    out.chat_id = m[1].str();
    if (m[2].matched) {
        out.thread_id = m[2].str();
    }
    return out;
}

TelegramTopic parse_discord_target(std::string_view ref) {
    // Discord uses the same numeric-snowflake regex as Telegram.
    return parse_telegram_topic(ref);
}

// ---- Env variable lookups ----------------------------------------------

std::optional<CronAutoTarget> cron_auto_target_from_env(EnvLookup lookup) {
    if (lookup == nullptr) {
        return std::nullopt;
    }
    auto platform_raw = lookup("HERMES_CRON_AUTO_DELIVER_PLATFORM");
    auto chat_raw = lookup("HERMES_CRON_AUTO_DELIVER_CHAT_ID");
    if (!platform_raw.has_value() || !chat_raw.has_value()) {
        return std::nullopt;
    }
    std::string platform{to_lower(trim_ascii(*platform_raw))};
    std::string chat_id{trim_ascii(*chat_raw)};
    if (platform.empty() || chat_id.empty()) {
        return std::nullopt;
    }
    CronAutoTarget out{};
    out.platform = platform;
    out.chat_id = chat_id;
    if (auto tid = lookup("HERMES_CRON_AUTO_DELIVER_THREAD_ID")) {
        std::string t{trim_ascii(*tid)};
        if (!t.empty()) {
            out.thread_id = t;
        }
    }
    return out;
}

}  // namespace hermes::tools::send_message::depth
