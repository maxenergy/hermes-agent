// Depth port of hermes_cli/commands.py pure helpers.

#include "hermes/cli/commands_depth.hpp"

#include <algorithm>
#include <cctype>
#include <regex>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace hermes::cli::commands_depth {

namespace {

std::string to_lower(std::string_view s) {
    std::string out(s);
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

// Return true when lhs matches rhs after "-" → "_" normalisation on
// both sides.
bool name_matches_hyphen_norm(std::string_view lhs, std::string_view rhs) {
    if (lhs.size() != rhs.size()) return false;
    for (std::size_t i = 0; i < lhs.size(); ++i) {
        char a = lhs[i] == '-' ? '_' : lhs[i];
        char b = rhs[i] == '-' ? '_' : rhs[i];
        if (a != b) return false;
    }
    return true;
}

}  // namespace

std::string sanitize_telegram_name(std::string_view raw) {
    std::string name = to_lower(raw);
    for (auto& c : name) {
        if (c == '-') c = '_';
    }
    // Strip anything other than a-z 0-9 _.
    std::string filtered;
    filtered.reserve(name.size());
    for (char c : name) {
        if ((c >= 'a' && c <= 'z')
            || (c >= '0' && c <= '9')
            || c == '_') {
            filtered.push_back(c);
        }
    }
    // Collapse multiple underscores.
    std::string collapsed;
    collapsed.reserve(filtered.size());
    bool prev_underscore = false;
    for (char c : filtered) {
        if (c == '_') {
            if (!prev_underscore) collapsed.push_back('_');
            prev_underscore = true;
        } else {
            collapsed.push_back(c);
            prev_underscore = false;
        }
    }
    // Strip leading and trailing underscores.
    std::size_t start = 0;
    while (start < collapsed.size() && collapsed[start] == '_') ++start;
    std::size_t end = collapsed.size();
    while (end > start && collapsed[end - 1] == '_') --end;
    return collapsed.substr(start, end - start);
}

std::vector<std::pair<std::string, std::string>> clamp_command_names(
    const std::vector<std::pair<std::string, std::string>>& entries,
    const std::unordered_set<std::string>& reserved) {
    std::unordered_set<std::string> used(reserved);
    std::vector<std::pair<std::string, std::string>> result;
    for (const auto& [raw_name, desc] : entries) {
        std::string name = raw_name;
        if (name.size() > kCmdNameLimit) {
            std::string candidate = name.substr(0, kCmdNameLimit);
            if (used.count(candidate) != 0) {
                std::string prefix = name.substr(0, kCmdNameLimit - 1);
                bool found = false;
                for (int digit = 0; digit < 10; ++digit) {
                    std::string cand = prefix + static_cast<char>('0' + digit);
                    if (used.count(cand) == 0) {
                        candidate = cand;
                        found = true;
                        break;
                    }
                }
                if (!found) continue;  // all 10 slots taken → drop
            }
            name = candidate;
        }
        if (used.count(name) != 0) continue;
        used.insert(name);
        result.emplace_back(name, desc);
    }
    return result;
}

std::string build_description(const CommandSpec& cmd) {
    if (cmd.args_hint.empty()) return cmd.description;
    std::ostringstream oss;
    oss << cmd.description << " (usage: /" << cmd.name << " " << cmd.args_hint << ")";
    return oss.str();
}

bool is_gateway_available(const CommandSpec& cmd) {
    if (!cmd.cli_only) return true;
    if (!cmd.gateway_config_gate.empty()) {
        return cmd.gateway_config_gate_truthy;
    }
    return false;
}

std::string format_gateway_help_line(const CommandSpec& cmd) {
    std::ostringstream oss;
    oss << "`/" << cmd.name;
    if (!cmd.args_hint.empty()) {
        oss << " " << cmd.args_hint;
    }
    oss << "`";

    auto visible = filter_alias_noise(cmd.name, cmd.aliases);
    if (!visible.empty()) {
        oss << " -- " << cmd.description << " (alias: ";
        for (std::size_t i = 0; i < visible.size(); ++i) {
            if (i > 0) oss << ", ";
            oss << "`/" << visible[i] << "`";
        }
        oss << ")";
    } else {
        oss << " -- " << cmd.description;
    }
    return oss.str();
}

std::vector<std::pair<std::string, std::string>> telegram_bot_commands(
    const std::vector<CommandSpec>& registry) {
    std::vector<std::pair<std::string, std::string>> out;
    for (const auto& cmd : registry) {
        if (!is_gateway_available(cmd)) continue;
        std::string tg = sanitize_telegram_name(cmd.name);
        if (!tg.empty()) out.emplace_back(tg, cmd.description);
    }
    return out;
}

std::optional<std::vector<std::string>> extract_pipe_subcommands(
    std::string_view args_hint_in) {
    if (args_hint_in.empty()) return std::nullopt;
    std::string args_hint(args_hint_in);
    std::regex re(R"([a-z]+(?:\|[a-z]+)+)");
    std::smatch m;
    if (!std::regex_search(args_hint, m, re)) {
        return std::nullopt;
    }
    std::string match_str = m.str(0);
    std::vector<std::string> out;
    std::string cur;
    for (char c : match_str) {
        if (c == '|') { out.push_back(cur); cur.clear(); }
        else cur.push_back(c);
    }
    out.push_back(cur);
    return out;
}

std::vector<std::string> filter_alias_noise(
    std::string_view canonical_name,
    const std::vector<std::string>& aliases) {
    std::vector<std::string> out;
    out.reserve(aliases.size());
    for (const auto& a : aliases) {
        if (a == canonical_name) continue;
        if (name_matches_hyphen_norm(a, canonical_name)) continue;
        out.push_back(a);
    }
    return out;
}

}  // namespace hermes::cli::commands_depth
