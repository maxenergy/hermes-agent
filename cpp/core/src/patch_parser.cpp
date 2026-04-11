#include "hermes/core/patch_parser.hpp"

#include "hermes/core/strings.hpp"

#include <cstdlib>
#include <sstream>
#include <string>
#include <vector>

namespace hermes::core::patch_parser {

namespace {

// Parse `@@ -o[,oc] +n[,nc] @@` headers. Returns false if the line
// isn't a valid hunk header.
bool parse_hunk_header(const std::string& line, Hunk& out) {
    if (!hermes::core::strings::starts_with(line, "@@")) {
        return false;
    }
    const auto first_at = line.find('@', 2);
    if (first_at == std::string::npos) {
        return false;
    }
    // Body between the two `@@` markers.
    const auto body = line.substr(2, first_at - 2 - 1);
    // Expected form: ` -o[,oc] +n[,nc] `
    std::istringstream is(body);
    std::string old_tok, new_tok;
    if (!(is >> old_tok >> new_tok)) {
        return false;
    }
    if (old_tok.empty() || old_tok[0] != '-' || new_tok.empty() || new_tok[0] != '+') {
        return false;
    }
    auto parse_pair = [](std::string_view tok, int& start, int& count) -> bool {
        tok.remove_prefix(1);
        const auto comma = tok.find(',');
        try {
            if (comma == std::string_view::npos) {
                start = std::stoi(std::string(tok));
                count = 1;
            } else {
                start = std::stoi(std::string(tok.substr(0, comma)));
                count = std::stoi(std::string(tok.substr(comma + 1)));
            }
        } catch (...) {
            return false;
        }
        return true;
    };
    if (!parse_pair(old_tok, out.old_start, out.old_count)) {
        return false;
    }
    if (!parse_pair(new_tok, out.new_start, out.new_count)) {
        return false;
    }
    return true;
}

std::vector<std::string> split_lines(std::string_view text) {
    std::vector<std::string> out;
    std::size_t start = 0;
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '\n') {
            out.emplace_back(text.substr(start, i - start));
            start = i + 1;
        }
    }
    if (start < text.size()) {
        out.emplace_back(text.substr(start));
    }
    return out;
}

std::string strip_diff_prefix(const std::string& raw) {
    // Accept `a/path`, `b/path`, or plain `path`.
    if (raw.size() >= 2 && (raw[0] == 'a' || raw[0] == 'b') && raw[1] == '/') {
        return raw.substr(2);
    }
    return raw;
}

}  // namespace

std::vector<FileDiff> parse_unified_diff(std::string_view text) {
    std::vector<FileDiff> out;
    const auto lines = split_lines(text);

    FileDiff* current = nullptr;
    Hunk* current_hunk = nullptr;

    for (std::size_t i = 0; i < lines.size(); ++i) {
        const auto& line = lines[i];
        if (hermes::core::strings::starts_with(line, "--- ")) {
            out.emplace_back();
            current = &out.back();
            current_hunk = nullptr;
            auto rest = hermes::core::strings::trim(line.substr(4));
            // Drop a trailing `\t<timestamp>` if present.
            if (const auto tab = rest.find('\t'); tab != std::string::npos) {
                rest = rest.substr(0, tab);
            }
            current->old_path = strip_diff_prefix(rest);
        } else if (hermes::core::strings::starts_with(line, "+++ ")) {
            if (current == nullptr) {
                // Missing `---` line — fabricate a FileDiff so we don't
                // drop the hunks on the floor.
                out.emplace_back();
                current = &out.back();
                current_hunk = nullptr;
            }
            auto rest = hermes::core::strings::trim(line.substr(4));
            if (const auto tab = rest.find('\t'); tab != std::string::npos) {
                rest = rest.substr(0, tab);
            }
            current->new_path = strip_diff_prefix(rest);
        } else if (hermes::core::strings::starts_with(line, "@@")) {
            if (current == nullptr) {
                // Diff body without a file header — still parse it
                // into a FileDiff with empty paths.
                out.emplace_back();
                current = &out.back();
            }
            current->hunks.emplace_back();
            current_hunk = &current->hunks.back();
            if (!parse_hunk_header(line, *current_hunk)) {
                current->hunks.pop_back();
                current_hunk = nullptr;
            }
        } else if (current_hunk != nullptr) {
            // Content line: ` `, `+`, `-`, or `\` (no newline at EOF).
            if (!line.empty() && (line[0] == ' ' || line[0] == '+' ||
                                  line[0] == '-' || line[0] == '\\')) {
                current_hunk->lines.push_back(line);
            } else if (line.empty()) {
                // Git-style empty context line — treat as single space.
                current_hunk->lines.emplace_back(" ");
            } else {
                // Unknown line — stop accumulating into this hunk.
                current_hunk = nullptr;
            }
        }
        // Else: pre-header metadata (`diff --git`, `index`, etc.) —
        // intentionally ignored.
    }
    return out;
}

}  // namespace hermes::core::patch_parser
