#include "hermes/core/env.hpp"

#include "hermes/core/strings.hpp"

#include <cctype>
#include <cstdlib>
#include <fstream>
#include <string>

namespace hermes::core::env {

bool is_truthy_value(std::string_view value) {
    const auto lower = hermes::core::strings::to_lower(
        hermes::core::strings::trim(value));
    return lower == "1" || lower == "true" || lower == "yes" || lower == "on";
}

bool env_var_enabled(std::string_view name) {
    const std::string key(name);
    const char* raw = std::getenv(key.c_str());
    if (raw == nullptr) {
        return false;
    }
    return is_truthy_value(raw);
}

namespace {

// Minimal `${VAR}` / `$VAR` expansion. Also honours dotenv values that
// were *already* set in the environment when the file declared the
// same key (we don't overwrite existing env vars — matches the common
// `python-dotenv` `override=False` default).
std::string expand(const std::string& input) {
    std::string out;
    out.reserve(input.size());
    for (std::size_t i = 0; i < input.size();) {
        const char c = input[i];
        if (c == '\\' && i + 1 < input.size()) {
            out.push_back(input[i + 1]);
            i += 2;
            continue;
        }
        if (c != '$') {
            out.push_back(c);
            ++i;
            continue;
        }
        // $VAR or ${VAR}
        std::size_t j = i + 1;
        std::string name;
        if (j < input.size() && input[j] == '{') {
            ++j;
            while (j < input.size() && input[j] != '}') {
                name.push_back(input[j++]);
            }
            if (j < input.size() && input[j] == '}') {
                ++j;
            }
        } else {
            while (j < input.size() &&
                   (std::isalnum(static_cast<unsigned char>(input[j])) ||
                    input[j] == '_')) {
                name.push_back(input[j++]);
            }
        }
        if (!name.empty()) {
            if (const char* v = std::getenv(name.c_str()); v != nullptr) {
                out.append(v);
            }
            i = j;
        } else {
            out.push_back('$');
            ++i;
        }
    }
    return out;
}

std::string unquote(std::string value) {
    if (value.size() >= 2) {
        const char first = value.front();
        const char last = value.back();
        if ((first == '"' && last == '"') || (first == '\'' && last == '\'')) {
            return value.substr(1, value.size() - 2);
        }
    }
    return value;
}

}  // namespace

void load_dotenv(const std::filesystem::path& path) {
    std::ifstream in(path);
    if (!in) {
        return;
    }
    std::string line;
    while (std::getline(in, line)) {
        auto trimmed = hermes::core::strings::trim(line);
        if (trimmed.empty() || trimmed.front() == '#') {
            continue;
        }
        if (hermes::core::strings::starts_with(trimmed, "export ")) {
            trimmed = hermes::core::strings::trim(trimmed.substr(7));
        }
        const auto eq = trimmed.find('=');
        if (eq == std::string::npos) {
            continue;
        }
        auto key = hermes::core::strings::trim(std::string_view(trimmed).substr(0, eq));
        auto value = hermes::core::strings::trim(std::string_view(trimmed).substr(eq + 1));

        const bool single_quoted = !value.empty() && value.front() == '\'' &&
                                   value.back() == '\'' && value.size() >= 2;
        const bool double_quoted = !value.empty() && value.front() == '"' &&
                                   value.back() == '"' && value.size() >= 2;
        // Strip inline comments only for unquoted values.
        if (!single_quoted && !double_quoted) {
            if (const auto hash = value.find(" #"); hash != std::string::npos) {
                value = hermes::core::strings::trim(value.substr(0, hash));
            }
        }
        value = unquote(value);
        // Expand variables unless the value was single-quoted
        // (matches the common python-dotenv behaviour).
        if (!single_quoted) {
            value = expand(value);
        }

        if (key.empty()) {
            continue;
        }
        // Don't override existing env vars — python-dotenv parity.
        if (std::getenv(key.c_str()) == nullptr) {
            ::setenv(key.c_str(), value.c_str(), 1);
        }
    }
}

}  // namespace hermes::core::env
