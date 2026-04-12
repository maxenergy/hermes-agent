#include "hermes/approval/command_scanner.hpp"

#include "hermes/core/ansi_strip.hpp"

#include <algorithm>
#include <cctype>
#include <regex>
#include <string>
#include <utility>

namespace hermes::approval {

namespace {

// Fold ASCII fullwidth Latin letters / digits / punctuation
// (U+FF01..U+FF5E) to plain ASCII. Lightweight NFKC fallback that
// handles the common obfuscation case without an ICU dependency.
std::string fold_fullwidth(std::string_view input) {
    std::string out;
    out.reserve(input.size());

    for (std::size_t i = 0; i < input.size();) {
        unsigned char c0 = static_cast<unsigned char>(input[i]);

        // Fast path: ASCII passes through unchanged.
        if (c0 < 0x80) {
            out.push_back(static_cast<char>(c0));
            ++i;
            continue;
        }

        // EF BC ?? — U+FF00..U+FF3F
        // EF BD ?? — U+FF40..U+FF7F
        if (c0 == 0xEF && i + 2 < input.size()) {
            unsigned char c1 = static_cast<unsigned char>(input[i + 1]);
            unsigned char c2 = static_cast<unsigned char>(input[i + 2]);
            if (c1 == 0xBC && c2 >= 0x81 && c2 <= 0xBF) {
                // U+FF01..U+FF3F  →  ASCII  0x21..0x5F
                out.push_back(static_cast<char>(0x21 + (c2 - 0x81)));
                i += 3;
                continue;
            }
            if (c1 == 0xBD && c2 >= 0x80 && c2 <= 0x9E) {
                // U+FF40..U+FF5E  →  ASCII  0x60..0x7E
                out.push_back(static_cast<char>(0x60 + (c2 - 0x80)));
                i += 3;
                continue;
            }
        }

        // Drop other non-ASCII bytes (control characters and unrelated
        // codepoints can't trip ASCII regex patterns anyway).
        ++i;
    }
    return out;
}

std::string collapse_whitespace(std::string_view input) {
    std::string out;
    out.reserve(input.size());
    bool in_ws = false;
    for (char c : input) {
        const bool is_ws = (c == ' ' || c == '\t' || c == '\n' || c == '\r' ||
                            c == '\v' || c == '\f');
        if (is_ws) {
            if (!in_ws && !out.empty()) {
                out.push_back(' ');
            }
            in_ws = true;
        } else {
            out.push_back(c);
            in_ws = false;
        }
    }
    while (!out.empty() && out.back() == ' ') {
        out.pop_back();
    }
    return out;
}

std::string to_lower(std::string s) {
    for (auto& c : s) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return s;
}

}  // namespace

std::string normalize_command(std::string_view command) {
    // 1. Strip ANSI escapes (ECMA-48)
    std::string s = hermes::core::ansi_strip::strip_ansi(command);

    // 2. Drop null bytes
    s.erase(std::remove(s.begin(), s.end(), '\0'), s.end());

    // 3. Lightweight NFKC: fold fullwidth latin to ASCII
    s = fold_fullwidth(s);

    // 4. Collapse whitespace
    s = collapse_whitespace(s);

    // 5. Lowercase
    s = to_lower(std::move(s));

    return s;
}

CommandScanner::CommandScanner() {
    const auto& table = danger_patterns();
    compiled_.reserve(table.size());
    for (const auto& p : table) {
        try {
            std::regex rx(p.regex,
                          std::regex::ECMAScript | std::regex::icase |
                              std::regex::optimize);
            compiled_.emplace_back(p, std::move(rx));
        } catch (const std::regex_error&) {
            // Skip patterns that fail to compile so a single bad regex
            // can't disable the entire scanner. Each std::regex flavor
            // is slightly different — if a pattern is critical it should
            // be exercised by the test suite below.
        }
    }
}

std::vector<Match> CommandScanner::scan(std::string_view command) const {
    std::vector<Match> hits;
    const std::string normalized = normalize_command(command);
    for (const auto& [pat, rx] : compiled_) {
        std::smatch m;
        if (std::regex_search(normalized, m, rx)) {
            hits.push_back({pat.key, m.str(), pat.severity, pat.category,
                            pat.description});
        }
    }
    return hits;
}

bool CommandScanner::is_dangerous(std::string_view command) const {
    const std::string normalized = normalize_command(command);
    for (const auto& [pat, rx] : compiled_) {
        if (std::regex_search(normalized, rx)) {
            return true;
        }
    }
    return false;
}

}  // namespace hermes::approval
