// Implementation of hermes/tools/terminal_tool_depth.hpp.
#include "hermes/tools/terminal_tool_depth.hpp"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdlib>
#include <limits>
#include <regex>
#include <sstream>
#include <string>
#include <unordered_map>

namespace hermes::tools::terminal::depth {

namespace {

std::string trim(std::string_view s) {
    std::size_t b = 0;
    std::size_t e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) {
        ++b;
    }
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) {
        --e;
    }
    return std::string{s.substr(b, e - b)};
}

std::string replace_newlines(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        out.push_back(c == '\n' || c == '\r' ? ' ' : c);
    }
    return out;
}

bool is_env_name_char(char c, bool first) {
    if (first) {
        return std::isalpha(static_cast<unsigned char>(c)) || c == '_';
    }
    return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
}

}  // namespace

// ---- Safe command previews ----------------------------------------------

std::string safe_command_preview(std::string_view command, std::size_t limit) {
    std::string s = replace_newlines(command);
    if (s.size() <= limit) {
        return s;
    }
    if (limit <= 3) {
        return std::string(limit, '.');
    }
    return s.substr(0, limit - 3) + "...";
}

// ---- Shell tokenisation -------------------------------------------------

bool looks_like_env_assignment(std::string_view token) {
    if (token.empty()) {
        return false;
    }
    if (token.front() == '=') {
        return false;
    }
    const auto eq = token.find('=');
    if (eq == std::string_view::npos) {
        return false;
    }
    const std::string_view name = token.substr(0, eq);
    if (name.empty() || !is_env_name_char(name.front(), /*first=*/true)) {
        return false;
    }
    for (std::size_t i = 1; i < name.size(); ++i) {
        if (!is_env_name_char(name[i], /*first=*/false)) {
            return false;
        }
    }
    return true;
}

std::pair<std::string, std::size_t> read_shell_token(std::string_view command,
                                                     std::size_t start) {
    std::size_t i = start;
    const std::size_t n = command.size();

    while (i < n) {
        const char ch = command[i];
        if (std::isspace(static_cast<unsigned char>(ch)) ||
            ch == ';' || ch == '|' || ch == '&' || ch == '(' || ch == ')') {
            break;
        }
        if (ch == '\'') {
            ++i;
            while (i < n && command[i] != '\'') {
                ++i;
            }
            if (i < n) {
                ++i;
            }
            continue;
        }
        if (ch == '"') {
            ++i;
            while (i < n) {
                const char inner = command[i];
                if (inner == '\\' && i + 1 < n) {
                    i += 2;
                    continue;
                }
                if (inner == '"') {
                    ++i;
                    break;
                }
                ++i;
            }
            continue;
        }
        if (ch == '\\' && i + 1 < n) {
            i += 2;
            continue;
        }
        ++i;
    }

    return {std::string{command.substr(start, i - start)}, i};
}

// ---- Sudo rewriter ------------------------------------------------------

SudoRewriteResult rewrite_real_sudo_invocations(std::string_view command) {
    std::string out;
    out.reserve(command.size() + 16);
    bool command_start = true;
    bool found = false;
    const std::size_t n = command.size();

    for (std::size_t i = 0; i < n;) {
        const char ch = command[i];

        if (std::isspace(static_cast<unsigned char>(ch))) {
            out.push_back(ch);
            if (ch == '\n') {
                command_start = true;
            }
            ++i;
            continue;
        }

        if (ch == '#' && command_start) {
            const auto nl = command.find('\n', i);
            if (nl == std::string_view::npos) {
                out.append(command.substr(i));
                i = n;
                break;
            }
            out.append(command.substr(i, nl - i));
            i = nl;
            continue;
        }

        if ((command.substr(i, 2) == "&&") ||
            (command.substr(i, 2) == "||") ||
            (command.substr(i, 2) == ";;")) {
            out.append(command.substr(i, 2));
            i += 2;
            command_start = true;
            continue;
        }

        if (ch == ';' || ch == '|' || ch == '&' || ch == '(') {
            out.push_back(ch);
            ++i;
            command_start = true;
            continue;
        }

        if (ch == ')') {
            out.push_back(ch);
            ++i;
            command_start = false;
            continue;
        }

        auto [token, next_i] = read_shell_token(command, i);
        if (command_start && token == "sudo") {
            out.append("sudo -S -p ''");
            found = true;
        } else {
            out.append(token);
        }

        if (command_start && looks_like_env_assignment(token)) {
            command_start = true;
        } else {
            command_start = false;
        }
        i = next_i;
    }

    return {out, found};
}

// ---- Exit-code interpretation ------------------------------------------

std::string last_pipeline_segment(std::string_view command) {
    // Split on ``&&``, ``||``, ``|``, ``;`` (with surrounding spaces).
    // Simple scan avoids pulling in extra regex state for repeated calls.
    std::size_t split_at = 0;
    const std::size_t n = command.size();
    for (std::size_t i = 0; i < n;) {
        const char c = command[i];
        if ((c == '|' && i + 1 < n && command[i + 1] == '|') ||
            (c == '&' && i + 1 < n && command[i + 1] == '&')) {
            i += 2;
            split_at = i;
            continue;
        }
        if (c == '|' || c == ';') {
            ++i;
            split_at = i;
            continue;
        }
        ++i;
    }
    return trim(command.substr(split_at));
}

std::string extract_base_command(std::string_view segment) {
    std::istringstream iss{std::string{segment}};
    std::string word;
    while (iss >> word) {
        if (word.find('=') != std::string::npos && !word.empty() &&
            word.front() != '-' && looks_like_env_assignment(word)) {
            continue;
        }
        // Trim trailing quotes / backticks.
        while (!word.empty() && (word.front() == '"' || word.front() == '\'' ||
                                 word.front() == '`')) {
            word.erase(word.begin());
        }
        const auto slash = word.find_last_of('/');
        if (slash != std::string::npos) {
            word = word.substr(slash + 1);
        }
        return word;
    }
    return {};
}

std::optional<std::string> interpret_exit_code(std::string_view command,
                                               int exit_code) {
    if (exit_code == 0) {
        return std::nullopt;
    }
    const std::string segment = last_pipeline_segment(command);
    const std::string base = extract_base_command(segment);
    if (base.empty()) {
        return std::nullopt;
    }

    static const std::unordered_map<std::string, std::unordered_map<int, std::string>>
        semantics = {
            {"grep", {{1, "No matches found (not an error)"}}},
            {"egrep", {{1, "No matches found (not an error)"}}},
            {"fgrep", {{1, "No matches found (not an error)"}}},
            {"rg", {{1, "No matches found (not an error)"}}},
            {"ag", {{1, "No matches found (not an error)"}}},
            {"ack", {{1, "No matches found (not an error)"}}},
            {"diff", {{1, "Files differ (expected, not an error)"}}},
            {"colordiff", {{1, "Files differ (expected, not an error)"}}},
            {"find",
             {{1,
               "Some directories were inaccessible (partial results may still "
               "be valid)"}}},
            {"test",
             {{1, "Condition evaluated to false (expected, not an error)"}}},
            {"[",
             {{1, "Condition evaluated to false (expected, not an error)"}}},
            {"curl",
             {{6, "Could not resolve host"},
              {7, "Failed to connect to host"},
              {22,
               "HTTP response code indicated error (e.g. 404, 500)"},
              {28, "Operation timed out"}}},
            {"git",
             {{1,
               "Non-zero exit (often normal — e.g. 'git diff' returns 1 when "
               "files differ)"}}},
        };

    auto it = semantics.find(base);
    if (it == semantics.end()) {
        return std::nullopt;
    }
    auto code_it = it->second.find(exit_code);
    if (code_it == it->second.end()) {
        return std::nullopt;
    }
    return code_it->second;
}

// ---- Stdin-vs-PTY heuristic --------------------------------------------

bool command_requires_pipe_stdin(std::string_view command) {
    // Collapse whitespace and lower-case, matching the Python normalisation.
    std::string normalised;
    normalised.reserve(command.size());
    bool prev_space = false;
    for (char c : command) {
        if (std::isspace(static_cast<unsigned char>(c))) {
            if (!prev_space && !normalised.empty()) {
                normalised.push_back(' ');
            }
            prev_space = true;
        } else {
            normalised.push_back(static_cast<char>(
                std::tolower(static_cast<unsigned char>(c))));
            prev_space = false;
        }
    }
    if (!normalised.empty() && normalised.back() == ' ') {
        normalised.pop_back();
    }
    const std::string prefix = "gh auth login";
    if (normalised.size() < prefix.size()) {
        return false;
    }
    if (normalised.compare(0, prefix.size(), prefix) != 0) {
        return false;
    }
    return normalised.find("--with-token") != std::string::npos;
}

// ---- Disk-usage warning -------------------------------------------------

DiskWarningOutput evaluate_disk_warning(const DiskWarningInput& in) {
    DiskWarningOutput out{};
    if (in.already_warned_today) {
        return out;
    }
    if (in.total_gb < in.threshold_gb) {
        return out;
    }
    out.should_warn = true;
    std::ostringstream oss;
    oss << "Hermes scratch directories are using " << std::fixed;
    oss.precision(1);
    oss << in.total_gb << " GB (threshold: " << in.threshold_gb
        << " GB). Run ``hermes doctor --clean`` to reclaim space.";
    out.message = oss.str();
    return out;
}

// ---- Foreground timeout clamp ------------------------------------------

int clamp_foreground_timeout(int requested, int default_value, int hard_cap) {
    if (hard_cap <= 0) {
        hard_cap = default_value > 0 ? default_value : 1;
    }
    int value = requested;
    if (value <= 0) {
        value = default_value;
    }
    if (value <= 0) {
        value = 1;
    }
    if (value > hard_cap) {
        value = hard_cap;
    }
    return value;
}

// ---- Env var parsing ---------------------------------------------------

int parse_env_int(const std::string& raw, int fallback) {
    if (raw.empty()) {
        return fallback;
    }
    try {
        std::size_t pos = 0;
        const long long v = std::stoll(raw, &pos);
        if (pos != raw.size()) {
            return fallback;
        }
        if (v > static_cast<long long>(std::numeric_limits<int>::max()) ||
            v < static_cast<long long>(std::numeric_limits<int>::min())) {
            return fallback;
        }
        return static_cast<int>(v);
    } catch (...) {
        return fallback;
    }
}

double parse_env_double(const std::string& raw, double fallback) {
    if (raw.empty()) {
        return fallback;
    }
    try {
        std::size_t pos = 0;
        const double v = std::stod(raw, &pos);
        if (pos != raw.size()) {
            return fallback;
        }
        return v;
    } catch (...) {
        return fallback;
    }
}

// ---- Command masking ---------------------------------------------------

MaskResult mask_secret_args(std::string_view command) {
    MaskResult out;
    std::string s{command};
    struct Pattern {
        std::regex re;
        std::string replacement;
    };
    // ``--token=XXX`` / ``--api-key=XXX``
    const std::vector<Pattern> patterns = {
        {std::regex{R"(--(?:token|api[-_]?key|password|secret)=\S+)",
                    std::regex::icase},
         "--REDACTED=***"},
        {std::regex{R"(--(?:token|api[-_]?key|password|secret)\s+\S+)",
                    std::regex::icase},
         "--REDACTED ***"},
        {std::regex{
             R"(\b(?:AWS_SECRET_ACCESS_KEY|AWS_ACCESS_KEY_ID|OPENAI_API_KEY|ANTHROPIC_API_KEY|GITHUB_TOKEN|SUDO_PASSWORD)=\S+)"},
         "REDACTED=***"},
    };

    for (const auto& p : patterns) {
        auto begin = std::sregex_iterator{s.begin(), s.end(), p.re};
        auto end = std::sregex_iterator{};
        for (auto it = begin; it != end; ++it) {
            ++out.replaced;
        }
        s = std::regex_replace(s, p.re, p.replacement);
    }
    out.redacted = std::move(s);
    return out;
}

}  // namespace hermes::tools::terminal::depth
