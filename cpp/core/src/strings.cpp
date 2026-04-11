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

}  // namespace hermes::core::strings
