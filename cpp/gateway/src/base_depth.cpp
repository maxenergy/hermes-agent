// Implementation of gateway/platforms/base.py pure helpers.

#include <hermes/gateway/base_depth.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <regex>
#include <sstream>

namespace hermes::gateway::base_depth {

// --- UTF-16 accounting ---------------------------------------------------

namespace {

// Decode a single UTF-8 code point starting at ``s[pos]``.  Returns
// (codepoint, bytes_consumed).  Malformed sequences are emitted as
// U+FFFD replacement characters, consuming a single byte.
std::pair<std::uint32_t, std::size_t>
decode_utf8(std::string_view s, std::size_t pos) {
    using Ret = std::pair<std::uint32_t, std::size_t>;
    if (pos >= s.size()) {
        return Ret{0u, std::size_t{0}};
    }
    const auto byte0 = static_cast<unsigned char>(s[pos]);
    if (byte0 < 0x80) {
        return Ret{static_cast<std::uint32_t>(byte0), std::size_t{1}};
    }
    auto need = std::size_t{0};
    auto cp = std::uint32_t{0};
    if ((byte0 & 0xE0) == 0xC0) {
        need = 1;
        cp = byte0 & 0x1F;
    } else if ((byte0 & 0xF0) == 0xE0) {
        need = 2;
        cp = byte0 & 0x0F;
    } else if ((byte0 & 0xF8) == 0xF0) {
        need = 3;
        cp = byte0 & 0x07;
    } else {
        return Ret{0xFFFDu, std::size_t{1}};
    }
    if (pos + need >= s.size()) {
        return Ret{0xFFFDu, std::size_t{1}};
    }
    for (std::size_t k{1}; k <= need; ++k) {
        const auto b = static_cast<unsigned char>(s[pos + k]);
        if ((b & 0xC0) != 0x80) {
            return Ret{0xFFFDu, std::size_t{1}};
        }
        cp = (cp << 6) | (b & 0x3F);
    }
    return Ret{cp, need + 1};
}

}  // namespace

std::size_t utf16_len(std::string_view s) {
    auto count = std::size_t{0};
    auto pos = std::size_t{0};
    while (pos < s.size()) {
        const auto [cp, used] = decode_utf8(s, pos);
        pos += used > 0 ? used : 1;
        count += (cp >= 0x10000) ? 2 : 1;
    }
    return count;
}

std::string prefix_within_utf16_limit(std::string_view s, std::size_t limit) {
    // Walk codepoints in order, accumulating UTF-16 units; stop when
    // the next codepoint would overflow.  Returns the byte prefix.
    auto pos = std::size_t{0};
    auto units = std::size_t{0};
    while (pos < s.size()) {
        const auto [cp, used] = decode_utf8(s, pos);
        const auto incr = (cp >= 0x10000) ? std::size_t{2} : std::size_t{1};
        if (units + incr > limit) {
            break;
        }
        units += incr;
        pos += used > 0 ? used : 1;
    }
    return std::string{s.substr(0, pos)};
}

// --- URL parsing / sanitization -----------------------------------------

ParsedUrl parse_url(std::string_view url) {
    auto result = ParsedUrl{};
    // Find "://"
    const auto scheme_end = url.find("://");
    if (scheme_end == std::string_view::npos) {
        return result;
    }
    // scheme must be alpha + [a-z0-9+-.]*
    for (std::size_t i{0}; i < scheme_end; ++i) {
        const auto c = url[i];
        const auto ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                        (c >= '0' && c <= '9') || c == '+' || c == '-' ||
                        c == '.';
        if (!ok) {
            return result;
        }
    }
    if (scheme_end == 0) {
        return result;
    }
    result.scheme = std::string{url.substr(0, scheme_end)};
    // lowercase
    for (auto& c : result.scheme) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    const auto rest = url.substr(scheme_end + 3);
    // netloc ends at first '/', '?', or '#'
    const auto path_start = rest.find_first_of("/?#");
    if (path_start == std::string_view::npos) {
        result.netloc = std::string{rest};
        result.path.clear();
    } else {
        result.netloc = std::string{rest.substr(0, path_start)};
        if (rest[path_start] == '/') {
            const auto fragq = rest.find_first_of("?#", path_start);
            if (fragq == std::string_view::npos) {
                result.path = std::string{rest.substr(path_start)};
            } else {
                result.path = std::string{rest.substr(path_start, fragq - path_start)};
            }
        }
    }
    result.has_scheme_netloc = !result.netloc.empty();
    return result;
}

std::string safe_url_for_log(std::string_view url, std::size_t max_len) {
    if (max_len == 0) {
        return {};
    }
    if (url.empty()) {
        return {};
    }
    const auto parsed = parse_url(url);

    std::string safe;
    if (parsed.has_scheme_netloc) {
        // Strip potential embedded credentials (user:pass@host).
        auto netloc = parsed.netloc;
        const auto at = netloc.rfind('@');
        if (at != std::string::npos) {
            netloc = netloc.substr(at + 1);
        }
        auto base = parsed.scheme + "://" + netloc;
        const auto& path = parsed.path;
        if (!path.empty() && path != "/") {
            const auto slash = path.rfind('/');
            const auto basename = (slash == std::string::npos)
                                       ? path
                                       : path.substr(slash + 1);
            if (!basename.empty()) {
                safe = base + "/.../" + basename;
            } else {
                safe = base + "/...";
            }
        } else {
            safe = std::move(base);
        }
    } else {
        safe = std::string{url};
    }

    if (safe.size() <= max_len) {
        return safe;
    }
    if (max_len <= 3) {
        return std::string(max_len, '.');
    }
    return safe.substr(0, max_len - 3) + "...";
}

// --- Proxy resolution ----------------------------------------------------

static std::string strip(std::string_view s) {
    auto begin = std::size_t{0};
    auto end = s.size();
    while (begin < end && std::isspace(static_cast<unsigned char>(s[begin]))) {
        ++begin;
    }
    while (end > begin && std::isspace(static_cast<unsigned char>(s[end - 1]))) {
        --end;
    }
    return std::string{s.substr(begin, end - begin)};
}

std::optional<std::string> resolve_proxy_url(
    std::string_view platform_env_var, GetEnvFn getenv_fn) {
    if (getenv_fn == nullptr) {
        return std::nullopt;
    }
    auto probe = [&](const char* name) -> std::optional<std::string> {
        const auto value = strip(getenv_fn(name));
        if (value.empty()) {
            return std::nullopt;
        }
        return value;
    };
    if (!platform_env_var.empty()) {
        // Need a null-terminated buffer to pass into getenv_fn.
        const auto key = std::string{platform_env_var};
        if (auto v = probe(key.c_str()); v.has_value()) {
            return v;
        }
    }
    static constexpr std::array<const char*, 6> keys{
        "HTTPS_PROXY", "HTTP_PROXY", "ALL_PROXY",
        "https_proxy", "http_proxy", "all_proxy",
    };
    for (const auto* key : keys) {
        if (auto v = probe(key); v.has_value()) {
            return v;
        }
    }
    return std::nullopt;
}

ProxyKind classify_proxy(std::string_view url) {
    if (url.empty()) {
        return ProxyKind::None;
    }
    std::string lower{url.substr(0, std::min<std::size_t>(url.size(), 16))};
    for (auto& c : lower) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    if (lower.rfind("socks", 0) == 0) {
        return ProxyKind::Socks;
    }
    if (lower.rfind("http://", 0) == 0 || lower.rfind("https://", 0) == 0) {
        return ProxyKind::Http;
    }
    return ProxyKind::Unknown;
}

// --- Image / media detection --------------------------------------------

bool looks_like_image(const unsigned char* data, std::size_t len) {
    if (len < 4) {
        return false;
    }
    // PNG: 89 50 4E 47 0D 0A 1A 0A
    if (len >= 8 && data[0] == 0x89 && data[1] == 'P' && data[2] == 'N' &&
        data[3] == 'G' && data[4] == 0x0D && data[5] == 0x0A &&
        data[6] == 0x1A && data[7] == 0x0A) {
        return true;
    }
    // JPEG: FF D8 FF
    if (len >= 3 && data[0] == 0xFF && data[1] == 0xD8 && data[2] == 0xFF) {
        return true;
    }
    // GIF: "GIF87a" or "GIF89a"
    if (len >= 6 && data[0] == 'G' && data[1] == 'I' && data[2] == 'F' &&
        data[3] == '8' && (data[4] == '7' || data[4] == '9') &&
        data[5] == 'a') {
        return true;
    }
    // BMP: "BM"
    if (len >= 2 && data[0] == 'B' && data[1] == 'M') {
        return true;
    }
    // WEBP: "RIFF....WEBP"
    if (len >= 12 && data[0] == 'R' && data[1] == 'I' && data[2] == 'F' &&
        data[3] == 'F' && data[8] == 'W' && data[9] == 'E' &&
        data[10] == 'B' && data[11] == 'P') {
        return true;
    }
    return false;
}

bool is_animation_url(std::string_view url) {
    auto lower = std::string{url};
    const auto qpos = lower.find('?');
    if (qpos != std::string::npos) {
        lower.resize(qpos);
    }
    for (auto& c : lower) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return lower.size() >= 4 &&
           lower.compare(lower.size() - 4, 4, ".gif") == 0;
}

namespace {

std::string to_lower(std::string_view s) {
    std::string out{s};
    for (auto& c : out) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return out;
}

bool has_image_marker(std::string_view url) {
    static constexpr std::array<const char*, 8> markers{
        ".png",   ".jpg",   ".jpeg",          ".gif",
        ".webp",  "fal.media", "fal-cdn",     "replicate.delivery",
    };
    const auto lower = to_lower(url);
    for (const auto* m : markers) {
        const std::string_view mv{m};
        if (mv.size() <= 5 && mv.front() == '.') {
            if (lower.size() >= mv.size() &&
                lower.compare(lower.size() - mv.size(), mv.size(), mv) == 0) {
                return true;
            }
            if (lower.find(mv) != std::string::npos) {
                return true;
            }
        } else if (lower.find(mv) != std::string::npos) {
            return true;
        }
    }
    return false;
}

// Collapse 3+ newlines into exactly 2, then trim.
std::string collapse_blank_lines(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    auto run = std::size_t{0};
    for (auto c : s) {
        if (c == '\n') {
            ++run;
            if (run <= 2) {
                out.push_back('\n');
            }
        } else {
            run = 0;
            out.push_back(c);
        }
    }
    // Trim whitespace
    auto begin = std::size_t{0};
    while (begin < out.size() &&
           std::isspace(static_cast<unsigned char>(out[begin]))) {
        ++begin;
    }
    auto end = out.size();
    while (end > begin && std::isspace(static_cast<unsigned char>(out[end - 1]))) {
        --end;
    }
    return out.substr(begin, end - begin);
}

}  // namespace

std::pair<std::vector<std::pair<std::string, std::string>>, std::string>
extract_images(std::string_view content) {
    std::vector<std::pair<std::string, std::string>> images;
    const std::string input{content};

    // Markdown image: ![alt](url)
    const std::regex md_re{R"(!\[([^\]]*)\]\((https?://[^\s\)]+)\))"};
    // HTML img: <img src="url"> or <img src="url"></img> or <img src="url"/>
    const std::regex html_re{
        R"(<img\s+src=["']?(https?://[^\s"'<>]+)["']?\s*/?>\s*(?:</img>)?)"};

    std::unordered_map<std::string, bool> extracted;
    for (auto it = std::sregex_iterator{input.begin(), input.end(), md_re};
         it != std::sregex_iterator{}; ++it) {
        const auto& m = *it;
        const auto alt = m.str(1);
        const auto url = m.str(2);
        if (has_image_marker(url)) {
            images.emplace_back(url, alt);
            extracted[url] = true;
        }
    }
    for (auto it = std::sregex_iterator{input.begin(), input.end(), html_re};
         it != std::sregex_iterator{}; ++it) {
        const auto& m = *it;
        const auto url = m.str(1);
        images.emplace_back(url, std::string{});
        extracted[url] = true;
    }

    if (extracted.empty()) {
        return {images, input};
    }
    // Remove matches whose URL was extracted.
    auto strip = [&](const std::regex& re, std::string_view text,
                      std::size_t url_group) {
        std::string out;
        out.reserve(text.size());
        const std::string tmp{text};
        auto last = std::size_t{0};
        for (auto it = std::sregex_iterator{tmp.begin(), tmp.end(), re};
             it != std::sregex_iterator{}; ++it) {
            const auto& m = *it;
            const auto url = m.str(static_cast<int>(url_group));
            const auto pos = static_cast<std::size_t>(m.position());
            const auto len = static_cast<std::size_t>(m.length());
            out.append(tmp, last, pos - last);
            if (extracted.find(url) == extracted.end()) {
                out.append(tmp, pos, len);
            }
            last = pos + len;
        }
        out.append(tmp, last, std::string::npos);
        return out;
    };

    auto cleaned = strip(md_re, input, 2);
    cleaned = strip(html_re, cleaned, 1);
    cleaned = collapse_blank_lines(cleaned);
    return {images, cleaned};
}

std::pair<std::vector<std::pair<std::string, bool>>, std::string>
extract_media(std::string_view content) {
    std::vector<std::pair<std::string, bool>> media;
    std::string cleaned{content};

    // Check for [[audio_as_voice]] directive.
    const std::string tag = "[[audio_as_voice]]";
    bool has_voice_tag = false;
    for (auto pos = cleaned.find(tag); pos != std::string::npos;
         pos = cleaned.find(tag, pos)) {
        has_voice_tag = true;
        cleaned.erase(pos, tag.size());
    }

    // Extract MEDIA:<path> tags. Match a path ending in a known media
    // extension, optionally wrapped in backticks / quotes.
    const std::regex media_re{
        R"([`"']?MEDIA:\s*(?:(`[^`\n]+`|"[^"\n]+"|'[^'\n]+')|((?:~/|/)\S+?\.(?:png|jpe?g|gif|webp|mp4|mov|avi|mkv|webm|ogg|opus|mp3|wav|m4a))|(\S+))[`"']?)",
        std::regex::icase};

    std::string after_media;
    after_media.reserve(cleaned.size());
    auto last = std::size_t{0};
    bool any_match = false;
    for (auto it = std::sregex_iterator{cleaned.begin(), cleaned.end(),
                                          media_re};
         it != std::sregex_iterator{}; ++it) {
        const auto& m = *it;
        std::string path;
        if (m[1].matched) {
            path = m.str(1);
            if (path.size() >= 2 && path.front() == path.back() &&
                (path.front() == '`' || path.front() == '"' ||
                 path.front() == '\'')) {
                path = path.substr(1, path.size() - 2);
            }
        } else if (m[2].matched) {
            path = m.str(2);
        } else if (m[3].matched) {
            path = m.str(3);
        }
        // Trim leading/trailing quote-like chars.
        while (!path.empty() &&
               (path.front() == '`' || path.front() == '"' ||
                path.front() == '\'' || std::isspace(static_cast<unsigned char>(path.front())))) {
            path.erase(path.begin());
        }
        while (!path.empty() &&
               (path.back() == '`' || path.back() == '"' ||
                path.back() == '\'' || path.back() == ',' ||
                path.back() == '.' || path.back() == ';' ||
                path.back() == ':' || path.back() == ')' ||
                path.back() == '}' || path.back() == ']')) {
            path.pop_back();
        }
        if (!path.empty()) {
            media.emplace_back(path, has_voice_tag);
            any_match = true;
        }
        const auto pos = static_cast<std::size_t>(m.position());
        const auto len = static_cast<std::size_t>(m.length());
        after_media.append(cleaned, last, pos - last);
        last = pos + len;
    }
    after_media.append(cleaned, last, std::string::npos);

    if (any_match) {
        after_media = collapse_blank_lines(after_media);
    }
    return {media, after_media};
}

std::pair<std::vector<std::string>, std::string>
extract_local_files_raw(std::string_view content) {
    const std::string input{content};

    // Find code spans to skip.
    const std::regex fence_re{R"(```[^\n]*\n[\s\S]*?```)"};
    const std::regex inline_re{R"(`[^`\n]+`)"};
    std::vector<std::pair<std::size_t, std::size_t>> code_spans;
    for (auto it = std::sregex_iterator{input.begin(), input.end(), fence_re};
         it != std::sregex_iterator{}; ++it) {
        const auto& m = *it;
        code_spans.emplace_back(static_cast<std::size_t>(m.position()),
                                 static_cast<std::size_t>(m.position() + m.length()));
    }
    for (auto it = std::sregex_iterator{input.begin(), input.end(), inline_re};
         it != std::sregex_iterator{}; ++it) {
        const auto& m = *it;
        code_spans.emplace_back(static_cast<std::size_t>(m.position()),
                                 static_cast<std::size_t>(m.position() + m.length()));
    }
    auto in_code = [&](std::size_t pos) {
        for (const auto& [s, e] : code_spans) {
            if (pos >= s && pos < e) {
                return true;
            }
        }
        return false;
    };

    // Path regex: absolute or ~/ paths ending in known media extension.
    const std::regex path_re{
        R"((?:^|[^/:\w.])((?:~/|/)(?:[\w.\-]+/)*[\w.\-]+\.(?:png|jpg|jpeg|gif|webp|mp4|mov|avi|mkv|webm)))",
        std::regex::icase};

    std::vector<std::string> found;
    std::unordered_map<std::string, bool> seen;
    std::string cleaned = input;
    std::vector<std::string> to_remove;
    for (auto it = std::sregex_iterator{input.begin(), input.end(), path_re};
         it != std::sregex_iterator{}; ++it) {
        const auto& m = *it;
        const auto start = static_cast<std::size_t>(m.position(1));
        if (in_code(start)) {
            continue;
        }
        const auto path = m.str(1);
        if (seen.find(path) != seen.end()) {
            continue;
        }
        seen[path] = true;
        found.push_back(path);
        to_remove.push_back(path);
    }
    for (const auto& raw : to_remove) {
        for (auto pos = cleaned.find(raw); pos != std::string::npos;
             pos = cleaned.find(raw, pos)) {
            cleaned.erase(pos, raw.size());
        }
    }
    if (!to_remove.empty()) {
        cleaned = collapse_blank_lines(cleaned);
    }
    return {found, cleaned};
}

// --- Command parsing -----------------------------------------------------

bool is_command(std::string_view text) {
    return !text.empty() && text.front() == '/';
}

std::optional<std::string> get_command(std::string_view text) {
    if (!is_command(text)) {
        return std::nullopt;
    }
    // First token after /
    auto pos = std::size_t{1};
    while (pos < text.size() &&
           !std::isspace(static_cast<unsigned char>(text[pos]))) {
        ++pos;
    }
    if (pos == 1) {
        return std::nullopt;
    }
    std::string name{text.substr(1, pos - 1)};
    // Lowercase
    for (auto& c : name) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    // Strip @mention suffix.
    const auto at = name.find('@');
    if (at != std::string::npos) {
        name.resize(at);
    }
    // Reject paths.
    if (name.find('/') != std::string::npos) {
        return std::nullopt;
    }
    if (name.empty()) {
        return std::nullopt;
    }
    return name;
}

std::string get_command_args(std::string_view text) {
    if (!is_command(text)) {
        return std::string{text};
    }
    auto pos = std::size_t{0};
    while (pos < text.size() &&
           !std::isspace(static_cast<unsigned char>(text[pos]))) {
        ++pos;
    }
    // Skip the whitespace.
    while (pos < text.size() &&
           std::isspace(static_cast<unsigned char>(text[pos]))) {
        ++pos;
    }
    return std::string{text.substr(pos)};
}

// --- Retry / timeout classification -------------------------------------

bool is_retryable_error(std::string_view error_text) {
    if (error_text.empty()) {
        return false;
    }
    const auto lowered = to_lower(error_text);
    static constexpr std::array<const char*, 9> patterns{
        "connecterror",     "connectionerror", "connectionreset",
        "connectionrefused", "connecttimeout", "network",
        "broken pipe",      "remotedisconnected", "eoferror",
    };
    for (const auto* p : patterns) {
        if (lowered.find(p) != std::string::npos) {
            return true;
        }
    }
    return false;
}

bool is_timeout_error(std::string_view error_text) {
    if (error_text.empty()) {
        return false;
    }
    const auto lowered = to_lower(error_text);
    return lowered.find("timed out") != std::string::npos ||
           lowered.find("readtimeout") != std::string::npos ||
           lowered.find("writetimeout") != std::string::npos;
}

// --- Caption merge -------------------------------------------------------

namespace {

std::string trim_copy(std::string_view s) {
    auto begin = std::size_t{0};
    auto end = s.size();
    while (begin < end && std::isspace(static_cast<unsigned char>(s[begin]))) {
        ++begin;
    }
    while (end > begin && std::isspace(static_cast<unsigned char>(s[end - 1]))) {
        --end;
    }
    return std::string{s.substr(begin, end - begin)};
}

}  // namespace

std::string merge_caption(std::string_view existing_text,
                           std::string_view new_text) {
    if (existing_text.empty()) {
        return std::string{new_text};
    }
    const auto needle = trim_copy(new_text);
    // Split existing on \n\n and compare trimmed chunks.
    std::size_t start{0};
    while (start <= existing_text.size()) {
        const auto next = existing_text.find("\n\n", start);
        const auto end = (next == std::string_view::npos) ? existing_text.size() : next;
        const auto piece = trim_copy(existing_text.substr(start, end - start));
        if (piece == needle) {
            return std::string{existing_text};
        }
        if (next == std::string_view::npos) {
            break;
        }
        start = next + 2;
    }
    std::string merged{existing_text};
    merged += "\n\n";
    merged += std::string{new_text};
    return trim_copy(merged);
}

// --- truncate_message ---------------------------------------------------

namespace {

struct FenceScan {
    bool in_code;
    std::string lang;
};

// Walk the body, flipping in-code state each time we encounter a fence
// line.  The language tag follows the opening fence and runs to the
// next whitespace.
FenceScan scan_fences(std::string_view body, bool in_code_start,
                        std::string_view lang_start) {
    bool in_code = in_code_start;
    std::string lang{lang_start};
    std::size_t pos{0};
    while (pos < body.size()) {
        auto nl = body.find('\n', pos);
        if (nl == std::string_view::npos) {
            nl = body.size();
        }
        auto line = body.substr(pos, nl - pos);
        // strip
        auto stripped = trim_copy(line);
        if (stripped.rfind("```", 0) == 0) {
            if (in_code) {
                in_code = false;
                lang.clear();
            } else {
                in_code = true;
                auto tag = stripped.substr(3);
                // trim
                const auto trimmed = trim_copy(tag);
                const auto ws = trimmed.find_first_of(" \t");
                lang = (ws == std::string::npos) ? trimmed : trimmed.substr(0, ws);
            }
        }
        if (nl >= body.size()) {
            break;
        }
        pos = nl + 1;
    }
    return {in_code, lang};
}

}  // namespace

std::vector<std::string> truncate_message(std::string_view content,
                                            std::size_t max_length) {
    if (content.size() <= max_length) {
        return {std::string{content}};
    }

    constexpr std::size_t INDICATOR_RESERVE{10};
    const std::string FENCE_CLOSE{"\n```"};

    std::vector<std::string> chunks;
    std::string remaining{content};
    std::optional<std::string> carry_lang;

    while (!remaining.empty()) {
        std::string prefix;
        if (carry_lang.has_value()) {
            prefix = "```" + *carry_lang + "\n";
        }

        std::size_t headroom{0};
        if (max_length > INDICATOR_RESERVE + prefix.size() + FENCE_CLOSE.size()) {
            headroom = max_length - INDICATOR_RESERVE - prefix.size() - FENCE_CLOSE.size();
        } else {
            headroom = max_length / 2;
        }

        if (prefix.size() + remaining.size() <=
            max_length - INDICATOR_RESERVE) {
            chunks.push_back(prefix + remaining);
            break;
        }

        const auto region_len = std::min<std::size_t>(headroom, remaining.size());
        const auto region = std::string_view{remaining}.substr(0, region_len);
        auto split_at = region.rfind('\n');
        if (split_at == std::string_view::npos ||
            split_at < headroom / 2) {
            const auto sp = region.rfind(' ');
            if (sp != std::string_view::npos) {
                split_at = sp;
            }
        }
        if (split_at == std::string_view::npos || split_at < 1) {
            split_at = headroom;
        }

        // Inline-code safety.
        auto candidate = std::string_view{remaining}.substr(0, split_at);
        std::size_t backtick_count{0};
        for (std::size_t i{0}; i < candidate.size(); ++i) {
            if (candidate[i] == '`') {
                if (i == 0 || candidate[i - 1] != '\\') {
                    ++backtick_count;
                }
            }
        }
        if (backtick_count % 2 == 1) {
            auto last_bt = candidate.rfind('`');
            while (last_bt != std::string_view::npos && last_bt > 0 &&
                   candidate[last_bt - 1] == '\\') {
                last_bt = candidate.rfind('`', last_bt - 1);
            }
            if (last_bt != std::string_view::npos && last_bt > 0) {
                auto safe_split = candidate.rfind(' ', last_bt);
                const auto nl_split = candidate.rfind('\n', last_bt);
                if (nl_split != std::string_view::npos &&
                    (safe_split == std::string_view::npos || nl_split > safe_split)) {
                    safe_split = nl_split;
                }
                if (safe_split != std::string_view::npos &&
                    safe_split > headroom / 4) {
                    split_at = safe_split;
                }
            }
        }

        std::string chunk_body{remaining.substr(0, split_at)};
        remaining.erase(0, split_at);
        while (!remaining.empty() &&
               std::isspace(static_cast<unsigned char>(remaining.front()))) {
            remaining.erase(remaining.begin());
        }

        auto full_chunk = prefix + chunk_body;
        const auto scan = scan_fences(chunk_body,
                                       carry_lang.has_value(),
                                       carry_lang.value_or(""));
        if (scan.in_code) {
            full_chunk += FENCE_CLOSE;
            carry_lang = scan.lang;
        } else {
            carry_lang.reset();
        }
        chunks.push_back(full_chunk);
    }

    if (chunks.size() > 1) {
        const auto total = chunks.size();
        for (std::size_t i{0}; i < total; ++i) {
            chunks[i] += " (" + std::to_string(i + 1) + "/" +
                         std::to_string(total) + ")";
        }
    }
    return chunks;
}

// --- is_network_accessible ----------------------------------------------

namespace {

bool all_digits(std::string_view s) {
    if (s.empty()) {
        return false;
    }
    for (auto c : s) {
        if (!std::isdigit(static_cast<unsigned char>(c))) {
            return false;
        }
    }
    return true;
}

bool parse_ipv4(std::string_view host, std::array<int, 4>& out) {
    std::size_t start{0};
    std::size_t count{0};
    while (start <= host.size() && count < 4) {
        const auto dot = host.find('.', start);
        const auto end = (dot == std::string_view::npos) ? host.size() : dot;
        const auto piece = host.substr(start, end - start);
        if (!all_digits(piece) || piece.size() > 3) {
            return false;
        }
        const auto value = std::stoi(std::string{piece});
        if (value < 0 || value > 255) {
            return false;
        }
        out[count++] = value;
        if (dot == std::string_view::npos) {
            break;
        }
        start = dot + 1;
    }
    return count == 4 && start + 0 <= host.size();
}

}  // namespace

AddressKind classify_address(std::string_view host) {
    if (host.empty()) {
        return AddressKind::Invalid;
    }
    std::array<int, 4> parts{};
    if (parse_ipv4(host, parts)) {
        if (parts[0] == 127) {
            return AddressKind::Ipv4Loopback;
        }
        if (parts[0] == 0 && parts[1] == 0 && parts[2] == 0 && parts[3] == 0) {
            return AddressKind::Ipv4Unspecified;
        }
        return AddressKind::Ipv4Public;
    }
    // IPv6: minimal handling for common forms.
    if (host.find(':') == std::string_view::npos) {
        return AddressKind::Invalid;
    }
    const auto lower = to_lower(host);
    if (lower == "::1") {
        return AddressKind::Ipv6Loopback;
    }
    if (lower == "::" || lower == "0:0:0:0:0:0:0:0") {
        return AddressKind::Ipv6Unspecified;
    }
    if (lower.rfind("::ffff:", 0) == 0) {
        const auto rest = lower.substr(7);
        std::array<int, 4> ipv4{};
        if (parse_ipv4(rest, ipv4)) {
            if (ipv4[0] == 127) {
                return AddressKind::Ipv6MappedLoopback;
            }
            return AddressKind::Ipv6Public;
        }
    }
    if (lower.rfind("fe80:", 0) == 0 || lower.rfind("fc", 0) == 0 ||
        lower.rfind("fd", 0) == 0) {
        return AddressKind::Ipv6Public;  // link-local/ULA — still not loopback
    }
    return AddressKind::Ipv6Public;
}

bool is_network_accessible(std::string_view host) {
    const auto kind = classify_address(host);
    switch (kind) {
        case AddressKind::Ipv4Loopback:
        case AddressKind::Ipv6Loopback:
        case AddressKind::Ipv6MappedLoopback:
            return false;
        case AddressKind::Ipv4Unspecified:
        case AddressKind::Ipv6Unspecified:
        case AddressKind::Ipv4Public:
        case AddressKind::Ipv6Public:
            return true;
        case AddressKind::Invalid:
            break;
    }
    // Hostname — Python resolves via DNS.  We do not link against the
    // resolver here; treat common loopback aliases as non-accessible and
    // fall back to "accessible" for everything else so the helper is
    // conservative without platform dependencies.
    const auto lower = to_lower(host);
    if (lower == "localhost" || lower == "localhost.localdomain" ||
        lower == "ip6-localhost" || lower == "ip6-loopback") {
        return false;
    }
    // Fail-closed for obviously empty strings.
    if (lower.empty()) {
        return false;
    }
    return true;
}

}  // namespace hermes::gateway::base_depth
