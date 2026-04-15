#include "hermes/gateway/stream_consumer_text.hpp"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <regex>
#include <string>

namespace hermes::gateway {

namespace {

// Matches `"MEDIA:<path>"`, `'MEDIA:<path>'`, MEDIA:<path>, and
// backtick-wrapped variants — same as the Python regex.
const std::regex& media_tag_re() {
    static const std::regex re(R"([`"']?MEDIA:\s*\S+[`"']?)");
    return re;
}

// Collapse any run of 3+ newlines down to 2.
std::string collapse_blank_runs(std::string_view in) {
    std::string out;
    out.reserve(in.size());
    std::size_t run = 0;
    for (char c : in) {
        if (c == '\n') {
            ++run;
            if (run <= 2) out.push_back(c);
        } else {
            run = 0;
            out.push_back(c);
        }
    }
    return out;
}

std::string rstrip(std::string_view in) {
    std::size_t end = in.size();
    while (end > 0) {
        unsigned char c = static_cast<unsigned char>(in[end - 1]);
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\v' ||
            c == '\f') {
            --end;
        } else {
            break;
        }
    }
    return std::string(in.substr(0, end));
}

}  // namespace

bool has_media_directives(std::string_view text) {
    return text.find("MEDIA:") != std::string_view::npos ||
           text.find("[[audio_as_voice]]") != std::string_view::npos;
}

std::string clean_for_display(std::string_view text) {
    if (!has_media_directives(text)) return std::string(text);

    // Drop the audio-as-voice marker first.
    std::string working;
    working.reserve(text.size());
    const std::string marker = "[[audio_as_voice]]";
    std::size_t pos = 0;
    while (pos < text.size()) {
        auto idx = text.find(marker, pos);
        if (idx == std::string_view::npos) {
            working.append(text.substr(pos));
            break;
        }
        working.append(text.substr(pos, idx - pos));
        pos = idx + marker.size();
    }

    // Strip MEDIA:<path> tags using the regex.
    std::string stripped = std::regex_replace(working, media_tag_re(), "");

    // Collapse excessive blank lines left behind.
    std::string collapsed = collapse_blank_runs(stripped);

    // Trim trailing whitespace — preserve leading content.
    return rstrip(collapsed);
}

std::string continuation_text(std::string_view final_text,
                              std::string_view prefix) {
    if (!prefix.empty() &&
        final_text.size() >= prefix.size() &&
        final_text.compare(0, prefix.size(), prefix) == 0) {
        std::string_view tail = final_text.substr(prefix.size());
        // lstrip equivalent to Python's `str.lstrip()`.
        std::size_t i = 0;
        while (i < tail.size()) {
            unsigned char c = static_cast<unsigned char>(tail[i]);
            if (std::isspace(c)) {
                ++i;
            } else {
                break;
            }
        }
        return std::string(tail.substr(i));
    }
    return std::string(final_text);
}

std::string visible_prefix(std::string_view rendered, std::string_view cursor) {
    std::string_view view = rendered;
    if (!cursor.empty() && view.size() >= cursor.size() &&
        view.compare(view.size() - cursor.size(), cursor.size(), cursor) == 0) {
        view.remove_suffix(cursor.size());
    }
    return clean_for_display(view);
}

std::vector<std::string> split_text_chunks(std::string_view text,
                                            std::size_t limit) {
    std::vector<std::string> out;
    if (text.size() <= limit) {
        out.emplace_back(text);
        return out;
    }
    std::string remaining(text);
    while (remaining.size() > limit) {
        auto split_at = remaining.rfind('\n', limit);
        if (split_at == std::string::npos || split_at < limit / 2) {
            split_at = limit;
        }
        out.push_back(remaining.substr(0, split_at));
        // lstrip leading '\n' from the continuation.
        std::size_t start = split_at;
        while (start < remaining.size() && remaining[start] == '\n') ++start;
        remaining = remaining.substr(start);
    }
    if (!remaining.empty()) out.push_back(std::move(remaining));
    return out;
}

std::size_t safe_split_limit(std::size_t raw_limit, std::string_view cursor) {
    constexpr std::size_t kFormatBuffer = 100;
    constexpr std::size_t kFloor = 500;
    std::size_t reserve = cursor.size() + kFormatBuffer;
    if (raw_limit <= reserve) return kFloor;
    std::size_t candidate = raw_limit - reserve;
    return std::max(kFloor, candidate);
}

bool should_edit_now(bool got_done,
                     bool got_segment_break,
                     std::size_t accumulated_len,
                     std::chrono::duration<double> elapsed,
                     const StreamConsumerTextConfig& cfg) {
    if (got_done || got_segment_break) return true;
    if (accumulated_len >= cfg.buffer_threshold) return true;
    if (accumulated_len > 0 && elapsed >= cfg.edit_interval) return true;
    return false;
}

std::string render_intermediate_body(std::string_view accumulated,
                                     bool got_done,
                                     bool got_segment_break,
                                     std::string_view cursor) {
    std::string out(accumulated);
    if (!got_done && !got_segment_break) out.append(cursor);
    return out;
}

std::size_t compute_split_offset(std::string_view accumulated,
                                  std::size_t safe_limit) {
    if (accumulated.size() <= safe_limit) return accumulated.size();
    auto split_at = accumulated.rfind('\n', safe_limit);
    if (split_at == std::string_view::npos || split_at < safe_limit / 2) {
        split_at = safe_limit;
    }
    return split_at;
}

}  // namespace hermes::gateway
