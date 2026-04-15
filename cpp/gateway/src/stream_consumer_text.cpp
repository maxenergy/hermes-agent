#include "hermes/gateway/stream_consumer_text.hpp"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <optional>
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

// ---------------------------------------------------------------------------
// truncate_message depth port
// ---------------------------------------------------------------------------

namespace {

std::string lstrip(std::string_view s) {
    std::size_t i = 0;
    while (i < s.size() &&
           std::isspace(static_cast<unsigned char>(s[i]))) {
        ++i;
    }
    return std::string(s.substr(i));
}

std::string trim_spaces(std::string_view s) {
    std::size_t a = 0;
    while (a < s.size() &&
           std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    std::size_t b = s.size();
    while (b > a &&
           std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    return std::string(s.substr(a, b - a));
}

}  // namespace

SplitPoint find_chunk_split_point(std::string_view remaining,
                                  std::size_t headroom) {
    SplitPoint sp;
    std::string_view region = remaining.substr(0, std::min(headroom, remaining.size()));
    auto nl = region.rfind('\n');
    std::size_t split_at = std::string_view::npos;
    if (nl != std::string_view::npos && nl >= headroom / 2) {
        split_at = nl;
    } else {
        auto sp_pos = region.rfind(' ');
        if (sp_pos != std::string_view::npos) split_at = sp_pos;
    }
    if (split_at == std::string_view::npos || split_at < 1) {
        split_at = headroom;
    }

    // Avoid splitting inside an inline code span.
    std::string_view candidate = remaining.substr(0, split_at);
    std::size_t bt_count = 0;
    std::size_t escaped = 0;
    for (std::size_t i = 0; i < candidate.size(); ++i) {
        if (candidate[i] == '`') {
            if (i > 0 && candidate[i - 1] == '\\') {
                ++escaped;
            } else {
                ++bt_count;
            }
        }
    }
    // Mirror Python "candidate.count('`') - candidate.count('\\`')".
    std::size_t py_total = bt_count + escaped;  // raw count
    std::size_t py_effective = py_total - escaped;
    if (py_effective % 2 == 1) {
        // Find last unescaped backtick.
        std::size_t last_bt = std::string_view::npos;
        for (std::size_t i = candidate.size(); i > 0; --i) {
            if (candidate[i - 1] == '`' &&
                (i == 1 || candidate[i - 2] != '\\')) {
                last_bt = i - 1;
                break;
            }
        }
        if (last_bt != std::string_view::npos && last_bt > 0) {
            std::size_t sp_before = candidate.rfind(' ', last_bt - 1);
            std::size_t nl_before = candidate.rfind('\n', last_bt - 1);
            std::size_t safe = (sp_before == std::string_view::npos)
                                   ? nl_before
                                   : (nl_before == std::string_view::npos
                                          ? sp_before
                                          : std::max(sp_before, nl_before));
            if (safe != std::string_view::npos && safe > headroom / 4) {
                split_at = safe;
                sp.fixup_applied = true;
            }
        }
    }
    sp.split_at = split_at;
    return sp;
}

bool ends_inside_code_block(std::string_view chunk_body,
                            bool currently_in_code,
                            std::string& out_lang) {
    bool in_code = currently_in_code;
    std::string lang = currently_in_code ? out_lang : std::string{};
    std::size_t start = 0;
    while (start <= chunk_body.size()) {
        auto nl = chunk_body.find('\n', start);
        std::string_view line =
            (nl == std::string_view::npos)
                ? chunk_body.substr(start)
                : chunk_body.substr(start, nl - start);
        std::string stripped = trim_spaces(line);
        if (stripped.size() >= 3 && stripped.compare(0, 3, "```") == 0) {
            if (in_code) {
                in_code = false;
                lang.clear();
            } else {
                in_code = true;
                std::string tag = trim_spaces(std::string_view(stripped).substr(3));
                auto sp = tag.find(' ');
                lang = (sp == std::string::npos) ? tag : tag.substr(0, sp);
            }
        }
        if (nl == std::string_view::npos) break;
        start = nl + 1;
    }
    out_lang = lang;
    return in_code;
}

std::vector<std::string> truncate_message_with_fences(
    std::string_view content, std::size_t max_length) {
    std::vector<std::string> out;
    if (content.size() <= max_length) {
        out.emplace_back(content);
        return out;
    }

    constexpr std::size_t kIndicatorReserve = 10;
    constexpr const char* kFenceClose = "\n```";
    constexpr std::size_t kFenceCloseLen = 4;

    std::string remaining(content);
    // When non-empty, the previous chunk ended inside a code block with
    // this language tag and we must reopen a matching fence.
    std::optional<std::string> carry_lang_opt;

    while (!remaining.empty()) {
        std::string prefix;
        if (carry_lang_opt) {
            prefix = "```" + *carry_lang_opt + "\n";
        }

        std::size_t headroom;
        if (max_length <= kIndicatorReserve + prefix.size() + kFenceCloseLen) {
            headroom = max_length / 2;
        } else {
            headroom = max_length - kIndicatorReserve - prefix.size() - kFenceCloseLen;
        }
        if (headroom < 1) headroom = max_length / 2;

        // All remaining fits in one chunk.
        if (prefix.size() + remaining.size() <= max_length - kIndicatorReserve) {
            out.push_back(prefix + remaining);
            break;
        }

        auto sp = find_chunk_split_point(remaining, headroom);
        std::size_t split_at = sp.split_at;
        if (split_at > remaining.size()) split_at = remaining.size();

        std::string chunk_body = remaining.substr(0, split_at);
        remaining = lstrip(std::string_view(remaining).substr(split_at));

        std::string full_chunk = prefix + chunk_body;

        std::string lang = carry_lang_opt.value_or("");
        bool in_code = ends_inside_code_block(chunk_body,
                                              carry_lang_opt.has_value(), lang);
        if (in_code) {
            full_chunk.append(kFenceClose);
            carry_lang_opt = lang;
        } else {
            carry_lang_opt.reset();
        }
        out.push_back(std::move(full_chunk));
    }

    if (out.size() > 1) {
        std::size_t total = out.size();
        for (std::size_t i = 0; i < out.size(); ++i) {
            out[i].append(" (");
            out[i].append(std::to_string(i + 1));
            out[i].push_back('/');
            out[i].append(std::to_string(total));
            out[i].push_back(')');
        }
    }
    return out;
}

}  // namespace hermes::gateway
