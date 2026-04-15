// Implementation of hermes/tools/terminal_tool_depth_ex.hpp.
#include "hermes/tools/terminal_tool_depth_ex.hpp"

#include <algorithm>
#include <cctype>
#include <regex>
#include <sstream>
#include <string>
#include <unordered_map>

namespace hermes::tools::terminal::depth_ex {

namespace {

std::string to_lower(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        out.push_back(static_cast<char>(
            std::tolower(static_cast<unsigned char>(c))));
    }
    return out;
}

std::string trim_ascii(std::string_view s) {
    std::size_t b{0};
    std::size_t e{s.size()};
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) {
        ++b;
    }
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) {
        --e;
    }
    return std::string{s.substr(b, e - b)};
}

bool is_shell_separator(char c) {
    return c == ';' || c == '|' || c == '&' || c == '(' || c == ')';
}

}  // namespace

// ---- Command preview ----------------------------------------------------

std::string safe_command_preview(std::optional<std::string_view> command,
                                 std::size_t limit) {
    if (!command.has_value()) {
        return "<None>";
    }
    std::string_view s{*command};
    if (s.size() > limit) {
        return std::string{s.substr(0, limit)};
    }
    return std::string{s};
}

// ---- Shell tokens -------------------------------------------------------

std::pair<std::string, std::size_t> read_shell_token(std::string_view command,
                                                     std::size_t start) {
    std::size_t i{start};
    std::size_t n{command.size()};
    while (i < n) {
        char ch{command[i]};
        if (std::isspace(static_cast<unsigned char>(ch)) ||
            is_shell_separator(ch)) {
            break;
        }
        if (ch == '\'') {
            ++i;
            while (i < n && command[i] != '\'') ++i;
            if (i < n) ++i;
            continue;
        }
        if (ch == '"') {
            ++i;
            while (i < n) {
                char inner{command[i]};
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

bool looks_like_env_assignment(std::string_view token) {
    auto eq = token.find('=');
    if (eq == std::string_view::npos || eq == 0) return false;
    std::string_view name{token.substr(0, eq)};
    if (name.empty()) return false;
    char c0{name[0]};
    if (!(c0 == '_' ||
          (c0 >= 'A' && c0 <= 'Z') ||
          (c0 >= 'a' && c0 <= 'z'))) {
        return false;
    }
    for (std::size_t i{1}; i < name.size(); ++i) {
        char c{name[i]};
        if (!(c == '_' ||
              (c >= 'A' && c <= 'Z') ||
              (c >= 'a' && c <= 'z') ||
              (c >= '0' && c <= '9'))) {
            return false;
        }
    }
    return true;
}

SudoRewriteResult rewrite_real_sudo_invocations(std::string_view command) {
    SudoRewriteResult out{};
    std::string acc{};
    std::size_t i{0};
    std::size_t n{command.size()};
    bool cmd_start{true};
    while (i < n) {
        char ch{command[i]};
        if (std::isspace(static_cast<unsigned char>(ch))) {
            acc.push_back(ch);
            if (ch == '\n') cmd_start = true;
            ++i;
            continue;
        }
        if (ch == '#' && cmd_start) {
            auto nl = command.find('\n', i);
            if (nl == std::string_view::npos) {
                acc.append(command.substr(i));
                break;
            }
            acc.append(command.substr(i, nl - i));
            i = nl;
            continue;
        }
        if (i + 1 < n &&
            ((command[i] == '&' && command[i + 1] == '&') ||
             (command[i] == '|' && command[i + 1] == '|') ||
             (command[i] == ';' && command[i + 1] == ';'))) {
            acc.append(command.substr(i, 2));
            i += 2;
            cmd_start = true;
            continue;
        }
        if (ch == ';' || ch == '|' || ch == '&' || ch == '(') {
            acc.push_back(ch);
            ++i;
            cmd_start = true;
            continue;
        }
        if (ch == ')') {
            acc.push_back(ch);
            ++i;
            cmd_start = false;
            continue;
        }
        auto [token, next_i] = read_shell_token(command, i);
        if (cmd_start && token == "sudo") {
            acc.append("sudo -S -p ''");
            out.rewrote = true;
        } else {
            acc.append(token);
        }
        if (cmd_start && looks_like_env_assignment(token)) {
            cmd_start = true;
        } else {
            cmd_start = false;
        }
        i = next_i;
    }
    out.command = std::move(acc);
    return out;
}

// ---- Exit-code interpretation -------------------------------------------

std::string last_command_segment(std::string_view command) {
    // Split on \s*(\|\||&&|[|;])\s*.  We do a manual scan.
    std::string s{command};
    std::size_t n{s.size()};
    std::size_t last{0};
    for (std::size_t i{0}; i + 1 < n; ++i) {
        if ((s[i] == '&' && s[i + 1] == '&') ||
            (s[i] == '|' && s[i + 1] == '|')) {
            last = i + 2;
            ++i;
            continue;
        }
        if (s[i] == '|' || s[i] == ';') {
            last = i + 1;
        }
    }
    std::string_view tail{s.c_str() + last, n - last};
    return trim_ascii(tail);
}

std::string extract_base_command(std::string_view segment) {
    std::string trimmed{trim_ascii(segment)};
    std::istringstream iss{trimmed};
    std::string word;
    while (iss >> word) {
        if (word.find('=') != std::string::npos && word.front() != '-') {
            // VAR=value — skip
            continue;
        }
        // Strip path prefix.
        auto slash = word.find_last_of("/\\");
        if (slash != std::string::npos) {
            word = word.substr(slash + 1);
        }
        return word;
    }
    return {};
}

std::string interpret_exit_code(std::string_view command, int exit_code) {
    if (exit_code == 0) return {};
    std::string segment{last_command_segment(command)};
    std::string base{extract_base_command(segment)};
    if (base.empty()) return {};

    // grep family
    static const std::vector<std::string> grep_family{
        "grep", "egrep", "fgrep", "rg", "ag", "ack"};
    for (const auto& g : grep_family) {
        if (base == g && exit_code == 1) {
            return "No matches found (not an error)";
        }
    }
    if ((base == "diff" || base == "colordiff") && exit_code == 1) {
        return "Files differ (expected, not an error)";
    }
    if (base == "find" && exit_code == 1) {
        return "Some directories were inaccessible (partial results may still "
               "be valid)";
    }
    if ((base == "test" || base == "[") && exit_code == 1) {
        return "Condition evaluated to false (expected, not an error)";
    }
    if (base == "curl") {
        switch (exit_code) {
            case 6: return "Could not resolve host";
            case 7: return "Failed to connect to host";
            case 22: return "HTTP response code indicated error (e.g. 404, 500)";
            case 28: return "Operation timed out";
            default: break;
        }
    }
    if (base == "git" && exit_code == 1) {
        return "Non-zero exit (often normal — e.g. 'git diff' returns 1 when "
               "files differ)";
    }
    return {};
}

// ---- PTY / stdin --------------------------------------------------------

bool command_requires_pipe_stdin(std::string_view command) {
    std::string lower{to_lower(command)};
    // Collapse runs of whitespace to a single space.
    std::string norm{};
    bool prev_space{false};
    for (char c : lower) {
        if (std::isspace(static_cast<unsigned char>(c))) {
            if (!prev_space && !norm.empty()) norm.push_back(' ');
            prev_space = true;
        } else {
            norm.push_back(c);
            prev_space = false;
        }
    }
    if (!norm.empty() && norm.back() == ' ') norm.pop_back();
    const std::string prefix{"gh auth login"};
    if (norm.size() < prefix.size() || norm.substr(0, prefix.size()) != prefix) {
        return false;
    }
    return norm.find("--with-token") != std::string::npos;
}

// ---- Workdir ------------------------------------------------------------

std::string validate_workdir(std::string_view workdir) {
    if (workdir.empty()) {
        return "Workdir must be a non-empty path.";
    }
    for (char c : workdir) {
        if (c == '\0') return "Workdir must not contain NUL bytes.";
    }
    // Reject shell metacharacters that could escape when joined.
    for (char c : workdir) {
        if (c == '$' || c == '`' || c == '\n' || c == '\r') {
            return "Workdir must not contain shell metacharacters.";
        }
    }
    return {};
}

// ---- Env parsing --------------------------------------------------------

int parse_env_int(std::optional<std::string_view> raw, int default_value) {
    if (!raw.has_value()) return default_value;
    std::string t{trim_ascii(*raw)};
    if (t.empty()) return default_value;
    std::size_t start{0};
    bool neg{false};
    if (t[0] == '+' || t[0] == '-') {
        if (t[0] == '-') neg = true;
        start = 1;
    }
    if (start >= t.size()) return default_value;
    long long acc{0};
    for (std::size_t i{start}; i < t.size(); ++i) {
        if (!std::isdigit(static_cast<unsigned char>(t[i]))) {
            return default_value;
        }
        acc = acc * 10 + (t[i] - '0');
        if (acc > 2'147'483'647LL) return default_value;
    }
    return static_cast<int>(neg ? -acc : acc);
}

bool parse_env_bool(std::optional<std::string_view> raw, bool default_value) {
    if (!raw.has_value()) return default_value;
    std::string t{to_lower(trim_ascii(*raw))};
    if (t.empty()) return default_value;
    if (t == "true" || t == "1" || t == "yes" || t == "on") return true;
    if (t == "false" || t == "0" || t == "no" || t == "off") return false;
    return default_value;
}

// ---- Env overrides ------------------------------------------------------

std::unordered_map<std::string, std::string> merge_env_overrides(
    const std::unordered_map<std::string, std::string>& base,
    const std::unordered_map<std::string, std::string>& overrides) {
    std::unordered_map<std::string, std::string> out{base};
    for (const auto& [k, v] : overrides) {
        if (v.empty()) {
            out.erase(k);
        } else {
            out[k] = v;
        }
    }
    return out;
}

// ---- Cleanup helpers ----------------------------------------------------

bool env_is_expired(double age_seconds, double lifetime_seconds) {
    return age_seconds >= lifetime_seconds;
}

std::vector<std::string> select_expired_env_ids(
    const std::vector<std::pair<std::string, double>>& envs,
    double lifetime_seconds) {
    std::vector<std::string> out{};
    for (const auto& [id, age] : envs) {
        if (env_is_expired(age, lifetime_seconds)) {
            out.push_back(id);
        }
    }
    return out;
}

// ---- Disk usage ---------------------------------------------------------

std::string format_disk_usage_warning(double used_pct, double threshold_pct) {
    if (used_pct < threshold_pct) return {};
    std::ostringstream os;
    os << "Disk usage warning: /tmp is " << static_cast<int>(used_pct)
       << "% full (threshold " << static_cast<int>(threshold_pct)
       << "%). Consider cleaning inactive sandboxes.";
    return os.str();
}

}  // namespace hermes::tools::terminal::depth_ex
