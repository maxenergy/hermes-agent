// C++17 port of @-reference parsing from agent/context_references.py.
#include "hermes/agent/at_references.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <regex>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

namespace hermes::agent::atref {

namespace fs = std::filesystem;

namespace {

// Sensitive paths — identical to Python's _SENSITIVE_* lists.
constexpr const char* k_sensitive_home_dirs[] = {
    ".ssh", ".aws", ".gnupg", ".kube", ".docker", ".azure", ".config/gh",
};
constexpr const char* k_sensitive_hermes_dirs[] = {
    "skills/.hub",
};
constexpr const char* k_sensitive_home_files[] = {
    ".ssh/authorized_keys",
    ".ssh/id_rsa",
    ".ssh/id_ed25519",
    ".ssh/config",
    ".bashrc",
    ".zshrc",
    ".profile",
    ".bash_profile",
    ".zprofile",
    ".netrc",
    ".pgpass",
    ".npmrc",
    ".pypirc",
};

bool is_word_or_slash(char c) {
    unsigned char u = static_cast<unsigned char>(c);
    return std::isalnum(u) || u == '_' || u == '/';
}

std::string rstrip_punct(std::string s) {
    static const std::string trailing = ",.;!?";
    while (!s.empty() && trailing.find(s.back()) != std::string::npos) {
        s.pop_back();
    }
    // Balance closing brackets.
    while (!s.empty() && (s.back() == ')' || s.back() == ']' || s.back() == '}')) {
        char closer = s.back();
        char opener = (closer == ')') ? '(' : (closer == ']') ? '[' : '{';
        int close_count = static_cast<int>(std::count(s.begin(), s.end(), closer));
        int open_count = static_cast<int>(std::count(s.begin(), s.end(), opener));
        if (close_count > open_count) {
            s.pop_back();
        } else {
            break;
        }
    }
    return s;
}

fs::path expanduser(const std::string& s) {
    if (s.empty() || s[0] != '~') return fs::path(s);
    const char* home = std::getenv("HOME");
    if (home == nullptr) return fs::path(s);
    if (s.size() == 1) return fs::path(home);
    if (s[1] == '/') return fs::path(std::string(home) + s.substr(1));
    return fs::path(s);
}

}  // namespace

std::string kind_name(RefKind k) {
    switch (k) {
        case RefKind::File: return "file";
        case RefKind::Folder: return "folder";
        case RefKind::Git: return "git";
        case RefKind::Diff: return "diff";
        case RefKind::Staged: return "staged";
        case RefKind::Url: return "url";
        case RefKind::Unknown: return "unknown";
    }
    return "unknown";
}

namespace detail {

std::string strip_trailing_punctuation(std::string value) {
    return rstrip_punct(std::move(value));
}

std::string strip_reference_wrappers(std::string value) {
    if (value.size() >= 2 &&
        (value.front() == '`' || value.front() == '"' || value.front() == '\'') &&
        value.front() == value.back()) {
        return value.substr(1, value.size() - 2);
    }
    return value;
}

void parse_file_reference_value(const std::string& value,
                                std::string& out_path,
                                std::optional<int>& out_start,
                                std::optional<int>& out_end) {
    out_start.reset();
    out_end.reset();

    // quoted: "path" or 'path' or `path`, optional :<start>[-<end>]
    static const std::regex quoted_re(
        R"(^(`|"|')(.+?)\1(?::(\d+)(?:-(\d+))?)?$)");
    std::smatch m;
    if (std::regex_match(value, m, quoted_re)) {
        out_path = m[2].str();
        if (m[3].matched) {
            out_start = std::stoi(m[3].str());
            out_end = m[4].matched ? std::stoi(m[4].str()) : *out_start;
        }
        return;
    }
    // unquoted with range
    static const std::regex range_re(R"(^(.+?):(\d+)(?:-(\d+))?$)");
    if (std::regex_match(value, m, range_re)) {
        out_path = m[1].str();
        out_start = std::stoi(m[2].str());
        out_end = m[3].matched ? std::stoi(m[3].str()) : *out_start;
        return;
    }
    out_path = strip_reference_wrappers(value);
}

}  // namespace detail

// ── Parser ────────────────────────────────────────────────────────────

// Instead of a single big regex (C++ std::regex lacks named groups and
// is slow), walk the string manually.
std::vector<AtReference> parse_at_references(const std::string& message) {
    std::vector<AtReference> out;
    if (message.empty()) return out;

    for (std::size_t i = 0; i < message.size(); ++i) {
        if (message[i] != '@') continue;
        // Must be preceded by a non-word, non-slash char (or start).
        if (i > 0 && is_word_or_slash(message[i - 1])) continue;

        // Try simple forms: @diff, @staged (must be followed by a word
        // boundary).
        auto match_simple = [&](const std::string& word,
                                RefKind k) -> bool {
            if (i + 1 + word.size() > message.size()) return false;
            if (message.compare(i + 1, word.size(), word) != 0) return false;
            const std::size_t end = i + 1 + word.size();
            if (end < message.size() && is_word_or_slash(message[end])) {
                return false;
            }
            AtReference r;
            r.raw = message.substr(i, end - i);
            r.kind = k;
            r.start = i;
            r.end = end;
            out.push_back(std::move(r));
            i = end - 1;  // advance (loop ++i)
            return true;
        };
        if (match_simple("diff", RefKind::Diff)) continue;
        if (match_simple("staged", RefKind::Staged)) continue;

        // Otherwise: @kind:value where kind is file/folder/git/url.
        struct KindSpec { const char* name; RefKind k; };
        const std::array<KindSpec, 4> kinds = {{
            {"file", RefKind::File},
            {"folder", RefKind::Folder},
            {"git", RefKind::Git},
            {"url", RefKind::Url},
        }};
        bool matched = false;
        for (const auto& spec : kinds) {
            const std::size_t name_len = std::string(spec.name).size();
            const std::size_t want_colon = i + 1 + name_len;
            if (want_colon >= message.size()) continue;
            if (message.compare(i + 1, name_len, spec.name) != 0) continue;
            if (message[want_colon] != ':') continue;

            // Value starts after the colon.
            std::size_t v_begin = want_colon + 1;
            std::size_t v_end;
            if (v_begin < message.size() &&
                (message[v_begin] == '`' || message[v_begin] == '"' ||
                 message[v_begin] == '\'')) {
                char q = message[v_begin];
                std::size_t close = message.find(q, v_begin + 1);
                if (close == std::string::npos) continue;
                v_end = close + 1;
                // Possibly followed by :<start>[-<end>]
                if (v_end < message.size() && message[v_end] == ':') {
                    std::size_t range_start = v_end + 1;
                    std::size_t p = range_start;
                    while (p < message.size() && std::isdigit(static_cast<unsigned char>(message[p]))) ++p;
                    if (p > range_start) {
                        if (p < message.size() && message[p] == '-') {
                            std::size_t p2 = p + 1;
                            while (p2 < message.size() && std::isdigit(static_cast<unsigned char>(message[p2]))) ++p2;
                            if (p2 > p + 1) v_end = p2;
                            else v_end = p;
                        } else {
                            v_end = p;
                        }
                    }
                }
            } else {
                v_end = v_begin;
                while (v_end < message.size() &&
                       !std::isspace(static_cast<unsigned char>(message[v_end]))) {
                    ++v_end;
                }
            }
            std::string value = message.substr(v_begin, v_end - v_begin);
            value = detail::strip_trailing_punctuation(value);
            if (value.empty()) continue;

            AtReference r;
            r.raw = message.substr(i, (v_begin + value.size()) - i);
            // Recompute end to match trimmed value.
            r.start = i;
            r.end = v_begin + value.size();
            r.kind = spec.k;
            if (spec.k == RefKind::File) {
                std::optional<int> ls, le;
                std::string target;
                detail::parse_file_reference_value(value, target, ls, le);
                r.target = target;
                r.line_start = ls;
                r.line_end = le;
            } else {
                r.target = detail::strip_reference_wrappers(value);
            }
            out.push_back(std::move(r));
            i = r.end - 1;
            matched = true;
            break;
        }
        (void)matched;
    }
    return out;
}

std::string remove_reference_tokens(const std::string& message,
                                    const std::vector<AtReference>& refs) {
    std::string out;
    out.reserve(message.size());
    std::size_t cursor = 0;
    for (const auto& r : refs) {
        out.append(message, cursor, r.start - cursor);
        cursor = r.end;
    }
    out.append(message, cursor, message.size() - cursor);

    // Collapse runs of whitespace to a single space.
    std::string collapsed;
    collapsed.reserve(out.size());
    bool prev_space = false;
    for (char c : out) {
        if (std::isspace(static_cast<unsigned char>(c))) {
            if (!prev_space) collapsed.push_back(' ');
            prev_space = true;
        } else {
            collapsed.push_back(c);
            prev_space = false;
        }
    }
    // Fix "text ," → "text,"
    static const std::regex space_punct(R"(\s+([,.;:!?]))");
    collapsed = std::regex_replace(collapsed, space_punct, "$1");
    // Trim.
    std::size_t b = 0, e = collapsed.size();
    while (b < e && std::isspace(static_cast<unsigned char>(collapsed[b]))) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(collapsed[e - 1]))) --e;
    return collapsed.substr(b, e - b);
}

fs::path resolve_reference_path(const fs::path& cwd,
                                const std::string& target,
                                const fs::path& allowed_root) {
    fs::path p = expanduser(target);
    if (!p.is_absolute()) p = cwd / p;
    std::error_code ec;
    fs::path resolved = fs::weakly_canonical(p, ec);
    if (ec) resolved = p;

    fs::path root = allowed_root.empty() ? cwd : allowed_root;
    fs::path resolved_root = fs::weakly_canonical(root, ec);
    if (ec) resolved_root = root;

    fs::path rel = fs::relative(resolved, resolved_root, ec);
    if (ec || rel.empty() || rel.native().find("..") != std::string::npos) {
        throw std::runtime_error("path is outside the allowed workspace");
    }
    return resolved;
}

void check_reference_path_allowed(const fs::path& path,
                                  const fs::path& home,
                                  const fs::path& hermes_home) {
    for (const char* rel : k_sensitive_home_files) {
        if (path == (home / rel)) {
            throw std::runtime_error(
                "path is a sensitive credential file and cannot be attached");
        }
    }
    if (path == (hermes_home / ".env")) {
        throw std::runtime_error(
            "path is a sensitive credential file and cannot be attached");
    }

    auto under = [&](const fs::path& dir) {
        std::error_code ec;
        fs::path rel = fs::relative(path, dir, ec);
        if (ec || rel.empty()) return false;
        return rel.native().find("..") == std::string::npos;
    };

    for (const char* rel : k_sensitive_home_dirs) {
        if (under(home / rel)) {
            throw std::runtime_error(
                "path is a sensitive credential or internal Hermes path and "
                "cannot be attached");
        }
    }
    for (const char* rel : k_sensitive_hermes_dirs) {
        if (under(hermes_home / rel)) {
            throw std::runtime_error(
                "path is a sensitive credential or internal Hermes path and "
                "cannot be attached");
        }
    }
}

}  // namespace hermes::agent::atref
