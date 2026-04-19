#include "hermes/core/strings.hpp"

#include <cctype>

namespace hermes::core::strings {

std::vector<std::string> split(std::string_view input, std::string_view delim) {
    std::vector<std::string> out;
    if (delim.empty()) {
        out.emplace_back(input);
        return out;
    }
    std::size_t pos = 0;
    while (true) {
        std::size_t next = input.find(delim, pos);
        if (next == std::string_view::npos) {
            out.emplace_back(input.substr(pos));
            break;
        }
        out.emplace_back(input.substr(pos, next - pos));
        pos = next + delim.size();
    }
    return out;
}

std::string join(const std::vector<std::string>& parts, std::string_view sep) {
    std::string out;
    for (std::size_t i = 0; i < parts.size(); ++i) {
        if (i != 0) {
            out.append(sep.data(), sep.size());
        }
        out.append(parts[i]);
    }
    return out;
}

bool starts_with(std::string_view input, std::string_view prefix) noexcept {
    return input.size() >= prefix.size() && input.compare(0, prefix.size(), prefix) == 0;
}

bool ends_with(std::string_view input, std::string_view suffix) noexcept {
    return input.size() >= suffix.size() &&
           input.compare(input.size() - suffix.size(), suffix.size(), suffix) == 0;
}

namespace {
bool is_ws(unsigned char c) noexcept {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f';
}
}  // namespace

std::string trim(std::string_view input) {
    std::size_t begin = 0;
    std::size_t end = input.size();
    while (begin < end && is_ws(static_cast<unsigned char>(input[begin]))) {
        ++begin;
    }
    while (end > begin && is_ws(static_cast<unsigned char>(input[end - 1]))) {
        --end;
    }
    return std::string(input.substr(begin, end - begin));
}

std::string to_lower(std::string_view input) {
    std::string out;
    out.reserve(input.size());
    for (char c : input) {
        auto uc = static_cast<unsigned char>(c);
        out.push_back(static_cast<char>(std::tolower(uc)));
    }
    return out;
}

std::string to_upper(std::string_view input) {
    std::string out;
    out.reserve(input.size());
    for (char c : input) {
        auto uc = static_cast<unsigned char>(c);
        out.push_back(static_cast<char>(std::toupper(uc)));
    }
    return out;
}

bool contains(std::string_view haystack, std::string_view needle) noexcept {
    if (needle.empty()) {
        return true;
    }
    return haystack.find(needle) != std::string_view::npos;
}

namespace {

// A UTF-8-encoded lone surrogate looks like:
//   byte0 = 0xED  (11101101)
//   byte1 = 0xA0..0xBF  (1010xxxx..1011xxxx  -> the high 4 bits of D8..DF)
//   byte2 = 0x80..0xBF  (10xxxxxx)
// Regular BMP codepoints whose 3-byte UTF-8 starts with 0xED have
// byte1 in 0x80..0x9F — those are valid and must NOT be replaced.
bool is_surrogate_at(std::string_view s, std::size_t i) noexcept {
    if (i + 2 >= s.size()) return false;
    const auto b0 = static_cast<unsigned char>(s[i]);
    const auto b1 = static_cast<unsigned char>(s[i + 1]);
    const auto b2 = static_cast<unsigned char>(s[i + 2]);
    return b0 == 0xED && b1 >= 0xA0 && b1 <= 0xBF &&
                         b2 >= 0x80 && b2 <= 0xBF;
}

}  // namespace

bool contains_surrogate(std::string_view input) noexcept {
    for (std::size_t i = 0; i + 2 < input.size(); ++i) {
        if (static_cast<unsigned char>(input[i]) != 0xED) continue;
        if (is_surrogate_at(input, i)) return true;
    }
    return false;
}

std::string sanitize_surrogates(std::string_view input) {
    // Fast-path: scan for 0xED first; if none, no surrogates possible.
    if (!contains_surrogate(input)) {
        return std::string(input);
    }
    std::string out;
    out.reserve(input.size());
    for (std::size_t i = 0; i < input.size();) {
        if (static_cast<unsigned char>(input[i]) == 0xED &&
            is_surrogate_at(input, i)) {
            // U+FFFD in UTF-8 = EF BF BD.
            out.push_back(static_cast<char>(0xEF));
            out.push_back(static_cast<char>(0xBF));
            out.push_back(static_cast<char>(0xBD));
            i += 3;
        } else {
            out.push_back(input[i]);
            ++i;
        }
    }
    return out;
}

}  // namespace hermes::core::strings
