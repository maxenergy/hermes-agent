// Pure-logic helpers for `hermes plugins` — ported from
// `hermes_cli/plugins_cmd.py`.
#include "hermes/cli/plugins_helpers.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <regex>
#include <sstream>
#include <stdexcept>

namespace hermes::cli::plugins_helpers {

namespace fs = std::filesystem;

namespace {

std::string trim(std::string s) {
    size_t i {0};
    while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) {
        ++i;
    }
    s.erase(0, i);
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) {
        s.pop_back();
    }
    return s;
}

std::string to_lower(std::string s) {
    for (char& c : s) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return s;
}

bool starts_with(const std::string& s, const std::string& prefix) {
    return s.size() >= prefix.size()
        && std::equal(prefix.begin(), prefix.end(), s.begin());
}

bool ends_with(const std::string& s, const std::string& suffix) {
    return s.size() >= suffix.size()
        && std::equal(suffix.rbegin(), suffix.rend(), s.rbegin());
}

std::vector<std::string> split(const std::string& s, char delim) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (c == delim) {
            out.push_back(cur);
            cur.clear();
        } else {
            cur.push_back(c);
        }
    }
    out.push_back(cur);
    return out;
}

// lexically normalise without touching the FS.
fs::path normalize(const fs::path& p) {
    return p.lexically_normal();
}

bool path_inside(const fs::path& candidate, const fs::path& root) {
    const fs::path c {normalize(candidate)};
    const fs::path r {normalize(root)};
    auto ci {c.begin()};
    auto ri {r.begin()};
    const auto ce {c.end()};
    const auto re {r.end()};
    for (; ri != re; ++ri, ++ci) {
        if (ci == ce) {
            return false;
        }
        if (*ci != *ri) {
            return false;
        }
    }
    // candidate must have at least one more component than root.
    return ci != ce;
}

}  // namespace

// ---------------------------------------------------------------------------

fs::path sanitize_plugin_name(
    const std::string& name,
    const fs::path& plugins_dir) {
    if (name.empty()) {
        throw std::invalid_argument {"Plugin name must not be empty."};
    }
    if (name == "." || name == "..") {
        throw std::invalid_argument {
            "Plugin name must not reference the plugins directory."};
    }
    for (const std::string& bad : {std::string {"/"},
                                    std::string {"\\"},
                                    std::string {".."}}) {
        if (name.find(bad) != std::string::npos) {
            throw std::invalid_argument {
                "Plugin name must not contain '" + bad + "'."};
        }
    }
    const fs::path target {normalize(plugins_dir / name)};
    const fs::path root {normalize(plugins_dir)};
    if (target == root) {
        throw std::invalid_argument {
            "Plugin name resolves to the plugins directory itself."};
    }
    if (!path_inside(target, root)) {
        throw std::invalid_argument {
            "Plugin name resolves outside the plugins directory."};
    }
    return target;
}

// ---------------------------------------------------------------------------

UrlScheme classify_url(const std::string& url) {
    if (starts_with(url, "https://")) {
        return UrlScheme::Https;
    }
    if (starts_with(url, "http://")) {
        return UrlScheme::Http;
    }
    if (starts_with(url, "ssh://") || starts_with(url, "git@")) {
        return UrlScheme::Ssh;
    }
    if (starts_with(url, "file://")) {
        return UrlScheme::File;
    }
    return UrlScheme::Unknown;
}

bool is_insecure_scheme(const std::string& url) {
    const UrlScheme s {classify_url(url)};
    return s == UrlScheme::Http || s == UrlScheme::File;
}

std::string resolve_git_url(const std::string& identifier) {
    const UrlScheme s {classify_url(identifier)};
    if (s != UrlScheme::Unknown) {
        return identifier;
    }
    // owner/repo shorthand.
    std::string trimmed {identifier};
    while (!trimmed.empty() && trimmed.front() == '/') {
        trimmed.erase(0, 1);
    }
    while (!trimmed.empty() && trimmed.back() == '/') {
        trimmed.pop_back();
    }
    const std::vector<std::string> parts {split(trimmed, '/')};
    if (parts.size() == 2 && !parts[0].empty() && !parts[1].empty()) {
        return "https://github.com/" + parts[0] + "/" + parts[1] + ".git";
    }
    throw std::invalid_argument {
        "Invalid plugin identifier: '" + identifier +
        "'. Use a Git URL or owner/repo shorthand."};
}

std::string repo_name_from_url(const std::string& url) {
    std::string name {url};
    while (!name.empty() && name.back() == '/') {
        name.pop_back();
    }
    if (ends_with(name, ".git")) {
        name = name.substr(0, name.size() - 4);
    }
    const auto slash {name.rfind('/')};
    if (slash != std::string::npos) {
        name = name.substr(slash + 1);
    }
    const auto colon {name.rfind(':')};
    if (colon != std::string::npos) {
        name = name.substr(colon + 1);
        const auto s2 {name.rfind('/')};
        if (s2 != std::string::npos) {
            name = name.substr(s2 + 1);
        }
    }
    return name;
}

// ---------------------------------------------------------------------------

bool manifest_version_supported(int manifest_version) {
    return manifest_version >= 1
        && manifest_version <= kSupportedManifestVersion;
}

std::optional<EnvSpec> parse_env_entry_string(const std::string& raw) {
    const std::string name {trim(raw)};
    if (name.empty()) {
        return std::nullopt;
    }
    return EnvSpec {name, {}, {}, false};
}

std::optional<EnvSpec> parse_env_entry_dict(
    const std::vector<std::pair<std::string, std::string>>& fields) {
    EnvSpec out {};
    for (const auto& [k, v] : fields) {
        if (k == "name") {
            out.name = trim(v);
        } else if (k == "description") {
            out.description = v;
        } else if (k == "url") {
            out.url = v;
        } else if (k == "secret") {
            const std::string lower {to_lower(trim(v))};
            out.secret = (lower == "true" || lower == "1" || lower == "yes");
        }
    }
    if (out.name.empty()) {
        return std::nullopt;
    }
    return out;
}

// ---------------------------------------------------------------------------

std::unordered_set<std::string> parse_disabled_set(
    const std::string& file_text) {
    std::unordered_set<std::string> out;
    std::stringstream ss {file_text};
    std::string line;
    while (std::getline(ss, line)) {
        const std::string cleaned {trim(line)};
        if (cleaned.empty() || cleaned.front() == '#') {
            continue;
        }
        out.insert(cleaned);
    }
    return out;
}

std::string serialise_disabled_set(
    const std::unordered_set<std::string>& set) {
    std::vector<std::string> names {set.begin(), set.end()};
    std::sort(names.begin(), names.end());
    std::string out;
    for (const std::string& n : names) {
        out += n;
        out += '\n';
    }
    return out;
}

// ---------------------------------------------------------------------------

std::optional<std::tuple<int, int, int>> parse_semver(
    const std::string& raw) {
    const std::string trimmed {trim(raw)};
    if (trimmed.empty()) {
        return std::nullopt;
    }
    const std::vector<std::string> parts {split(trimmed, '.')};
    if (parts.empty() || parts.size() > 3) {
        return std::nullopt;
    }
    std::array<int, 3> v {0, 0, 0};
    for (std::size_t i {0}; i < parts.size(); ++i) {
        if (parts[i].empty()) {
            return std::nullopt;
        }
        for (char c : parts[i]) {
            if (!std::isdigit(static_cast<unsigned char>(c))) {
                return std::nullopt;
            }
        }
        try {
            v[i] = std::stoi(parts[i]);
        } catch (...) {
            return std::nullopt;
        }
    }
    return std::make_tuple(v[0], v[1], v[2]);
}

std::optional<std::pair<std::string, std::tuple<int, int, int>>>
    parse_constraint(const std::string& raw) {
    const std::string trimmed {trim(raw)};
    if (trimmed.empty()) {
        return std::nullopt;
    }
    // Try longest-prefix-first so ">=" isn't chopped to ">".
    static const std::vector<std::string> ops {">=", "<=", "==", ">", "<"};
    for (const std::string& op : ops) {
        if (starts_with(trimmed, op)) {
            const std::string rest {trim(trimmed.substr(op.size()))};
            const auto ver {parse_semver(rest)};
            if (!ver) {
                return std::nullopt;
            }
            return std::make_pair(op, *ver);
        }
    }
    // No operator -> treat as "==".
    const auto ver {parse_semver(trimmed)};
    if (!ver) {
        return std::nullopt;
    }
    return std::make_pair(std::string {"=="}, *ver);
}

std::string plugin_status_label(PluginStatus s) {
    switch (s) {
        case PluginStatus::Enabled:
            return "enabled";
        case PluginStatus::Disabled:
            return "disabled";
        case PluginStatus::Broken:
            return "broken";
    }
    return "unknown";
}

std::string format_list_line(const std::string& name,
                             PluginStatus status,
                             const std::string& description) {
    std::string out {name};
    out += "  [";
    out += plugin_status_label(status);
    out += "]";
    if (!description.empty()) {
        out += "  \xE2\x80\x94 ";  // em dash
        out += description;
    }
    return out;
}

std::string example_target_name(const std::string& filename) {
    const std::string suffix {".example"};
    if (filename.size() <= suffix.size()) {
        return {};
    }
    if (!ends_with(filename, suffix)) {
        return {};
    }
    return filename.substr(0, filename.size() - suffix.size());
}

std::string scheme_warning(const std::string& url) {
    if (is_insecure_scheme(url)) {
        return "Warning: Using insecure/local URL scheme. "
               "Consider using https:// or git@ for production installs.";
    }
    return {};
}

std::string default_after_install_banner(const std::string& identifier,
                                         const std::string& plugin_dir) {
    std::string out;
    out += "Plugin installed: ";
    out += identifier;
    out += "\nLocation: ";
    out += plugin_dir;
    return out;
}

bool satisfies_constraint(const std::tuple<int, int, int>& current,
                          const std::string& constraint) {
    const auto parsed {parse_constraint(constraint)};
    if (!parsed) {
        return false;
    }
    const std::string& op {parsed->first};
    const auto& want {parsed->second};
    if (op == "==") {
        return current == want;
    }
    if (op == ">=") {
        return current >= want;
    }
    if (op == ">") {
        return current > want;
    }
    if (op == "<=") {
        return current <= want;
    }
    if (op == "<") {
        return current < want;
    }
    return false;
}

}  // namespace hermes::cli::plugins_helpers
