#include <hermes/gateway/gateway_helpers.hpp>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <random>
#include <sstream>

namespace hermes::gateway {

namespace {

std::string to_lower(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) out.push_back(static_cast<char>(std::tolower(
        static_cast<unsigned char>(c))));
    return out;
}

std::string trim(std::string_view s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    return std::string(s.substr(a, b - a));
}

constexpr const char* kHexDigits = "0123456789ABCDEF";

}  // namespace

// --- Media placeholder ---------------------------------------------------

std::string build_media_placeholder(const MediaPlaceholderEvent& event) {
    // Label derived from the message_type — matches the Python strings
    // used by _build_media_placeholder so the agent sees a stable prompt.
    auto mt = to_lower(event.message_type);
    const char* label = "a file";
    if (mt == "photo" || mt == "image") {
        label = "a photo";
    } else if (mt == "video") {
        label = "a video";
    } else if (mt == "audio") {
        label = "an audio recording";
    } else if (mt == "voice") {
        label = "a voice message";
    } else if (mt == "sticker" || mt == "animation" || mt == "gif") {
        label = "a sticker";
    } else if (mt == "document") {
        label = "a document";
    } else if (mt == "location") {
        label = "a location";
    } else if (mt == "contact") {
        label = "a contact card";
    }

    std::ostringstream os;
    os << "[user sent " << label;
    if (event.media_urls.size() > 1) {
        os << " (" << event.media_urls.size() << " items)";
    }
    if (!event.caption.empty()) {
        os << " with caption: " << event.caption;
    }
    os << "]";
    return os.str();
}

std::string build_media_placeholder(std::string_view message_type,
                                     std::string_view url) {
    MediaPlaceholderEvent ev;
    ev.message_type = std::string(message_type);
    if (!url.empty()) ev.media_urls.emplace_back(url);
    return build_media_placeholder(ev);
}

// --- safe_url_for_log ----------------------------------------------------

std::string safe_url_for_log(std::string_view url, std::size_t max_len) {
    if (url.empty()) return {};

    // Strip query (everything from '?').
    auto q = url.find('?');
    std::string_view base = q == std::string_view::npos ? url : url.substr(0, q);

    // Strip fragment.
    auto frag = base.find('#');
    if (frag != std::string_view::npos) base = base.substr(0, frag);

    // Strip user:pass@ segment between scheme and host.
    auto scheme_end = base.find("://");
    if (scheme_end != std::string_view::npos) {
        auto host_start = scheme_end + 3;
        auto at_pos = base.find('@', host_start);
        auto slash = base.find('/', host_start);
        if (at_pos != std::string_view::npos &&
            (slash == std::string_view::npos || at_pos < slash)) {
            // Remove credentials.
            std::string out;
            out.reserve(base.size());
            out.append(base.substr(0, host_start));
            out.append("***@");
            out.append(base.substr(at_pos + 1));
            if (out.size() > max_len) out = out.substr(0, max_len) + "...";
            return out;
        }
    }

    if (base.size() > max_len) {
        return std::string(base.substr(0, max_len)) + "...";
    }
    return std::string(base);
}

std::string mask_token(std::string_view token) {
    if (token.size() < 10) return "***";
    std::string out;
    out.reserve(token.size());
    out.append(token.substr(0, 4));
    out.append("***");
    out.append(token.substr(token.size() - 4));
    return out;
}

// --- truncate_message ----------------------------------------------------

std::vector<std::string> truncate_message(std::string_view content,
                                           std::size_t max_length) {
    std::vector<std::string> out;
    if (content.empty() || max_length == 0) return out;
    if (content.size() <= max_length) {
        out.emplace_back(content);
        return out;
    }

    size_t i = 0;
    while (i < content.size()) {
        size_t remaining = content.size() - i;
        if (remaining <= max_length) {
            out.emplace_back(content.substr(i));
            break;
        }
        // Prefer paragraph break, then sentence, then whitespace.
        size_t limit = i + max_length;
        size_t cut = std::string_view::npos;

        // Look for paragraph break.
        auto search_window = content.substr(i, max_length);
        auto para = search_window.rfind("\n\n");
        if (para != std::string_view::npos && para > max_length / 4) {
            cut = i + para + 2;
        } else {
            // Sentence end.
            size_t best = std::string_view::npos;
            for (size_t k = search_window.size(); k > max_length / 4; --k) {
                char c = search_window[k - 1];
                if (c == '.' || c == '!' || c == '?' || c == '\n') {
                    best = i + k;
                    break;
                }
            }
            if (best != std::string_view::npos) {
                cut = best;
            } else {
                // Whitespace.
                auto ws = search_window.rfind(' ');
                if (ws != std::string_view::npos && ws > max_length / 4) {
                    cut = i + ws + 1;
                } else {
                    cut = limit;
                }
            }
        }

        out.emplace_back(content.substr(i, cut - i));
        i = cut;
    }
    return out;
}

// --- human_delay ---------------------------------------------------------

std::chrono::milliseconds human_delay(std::uint64_t seed) {
    if (seed == 0) {
        seed = static_cast<std::uint64_t>(
            std::chrono::steady_clock::now().time_since_epoch().count());
    }
    std::mt19937_64 rng(seed);
    std::uniform_int_distribution<int> dist(800, 2400);
    return std::chrono::milliseconds(dist(rng));
}

// --- normalize / slash command helpers -----------------------------------

std::string normalize_chat_id(std::string_view raw) {
    auto trimmed = trim(raw);
    for (char c : trimmed) {
        if (static_cast<unsigned char>(c) < 0x20) return {};
    }
    return trimmed;
}

bool looks_like_slash_command(std::string_view content) {
    size_t i = 0;
    while (i < content.size() && std::isspace(static_cast<unsigned char>(content[i])))
        ++i;
    if (i >= content.size() || content[i] != '/') return false;
    // Must have a letter after the slash.
    if (i + 1 >= content.size() ||
        !std::isalpha(static_cast<unsigned char>(content[i + 1])))
        return false;
    return true;
}

std::optional<std::string> extract_command_word(std::string_view content) {
    if (!looks_like_slash_command(content)) return std::nullopt;
    size_t i = 0;
    while (std::isspace(static_cast<unsigned char>(content[i]))) ++i;
    ++i;  // skip slash
    size_t start = i;
    while (i < content.size() &&
           (std::isalnum(static_cast<unsigned char>(content[i])) ||
            content[i] == '_' || content[i] == '-')) {
        ++i;
    }
    if (start == i) return std::nullopt;
    return to_lower(content.substr(start, i - start));
}

std::string extract_command_args(std::string_view content) {
    if (!looks_like_slash_command(content)) return {};
    auto space = content.find(' ');
    if (space == std::string_view::npos) return {};
    return trim(content.substr(space + 1));
}

// --- percent_encode ------------------------------------------------------

std::string percent_encode(std::string_view value) {
    std::string out;
    out.reserve(value.size());
    for (unsigned char c : value) {
        bool unreserved =
            std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~';
        if (unreserved) {
            out.push_back(static_cast<char>(c));
        } else {
            out.push_back('%');
            out.push_back(kHexDigits[(c >> 4) & 0xF]);
            out.push_back(kHexDigits[c & 0xF]);
        }
    }
    return out;
}

// --- contents_equivalent -------------------------------------------------

bool contents_equivalent(std::string_view a, std::string_view b) {
    auto collapse = [](std::string_view s) {
        std::string out;
        out.reserve(s.size());
        bool prev_ws = false;
        for (char c : s) {
            auto uc = static_cast<unsigned char>(c);
            if (std::isspace(uc)) {
                if (!prev_ws && !out.empty()) out.push_back(' ');
                prev_ws = true;
            } else {
                out.push_back(static_cast<char>(std::tolower(uc)));
                prev_ws = false;
            }
        }
        while (!out.empty() && out.back() == ' ') out.pop_back();
        return out;
    };
    return collapse(a) == collapse(b);
}

}  // namespace hermes::gateway
