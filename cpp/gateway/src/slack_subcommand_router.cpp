// Phase 12 — Slack `/hermes` subcommand routing.
#include <hermes/gateway/slack_subcommand_router.hpp>

#include <algorithm>
#include <cctype>
#include <sstream>

namespace hermes::gateway {

namespace {

std::string trim(const std::string& s) {
    auto begin = std::find_if_not(s.begin(), s.end(),
                                  [](unsigned char c) { return std::isspace(c); });
    auto end = std::find_if_not(s.rbegin(), s.rend(),
                                [](unsigned char c) { return std::isspace(c); }).base();
    if (begin >= end) return "";
    return std::string(begin, end);
}

std::vector<std::string> split_ws(const std::string& s) {
    std::vector<std::string> out;
    std::istringstream iss(s);
    std::string tok;
    while (iss >> tok) out.push_back(std::move(tok));
    return out;
}

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    return s;
}

}  // namespace

std::optional<SlackSubcommand> parse_slack_subcommand(const std::string& text) {
    auto t = trim(text);
    if (t.empty()) return std::nullopt;

    auto sp = std::find_if(t.begin(), t.end(),
                           [](unsigned char c) { return std::isspace(c); });
    SlackSubcommand sc;
    sc.name = std::string(t.begin(), sp);
    if (!sc.name.empty() && sc.name[0] == '/') sc.name.erase(0, 1);
    sc.name = lower(std::move(sc.name));
    if (sp != t.end()) {
        sc.remainder = trim(std::string(sp, t.end()));
    }
    sc.argv = split_ws(sc.remainder);
    if (sc.name.empty()) return std::nullopt;
    return sc;
}

void SlackSubcommandRouter::register_handler(const std::string& name,
                                             Handler h) {
    handlers_[lower(name)] = std::move(h);
}

void SlackSubcommandRouter::set_known_commands(
    const std::map<std::string, std::string>& commands) {
    known_ = commands;
}

bool SlackSubcommandRouter::has_handler(const std::string& name) const {
    return handlers_.find(lower(name)) != handlers_.end();
}

std::string SlackSubcommandRouter::dispatch(const std::string& text) const {
    auto parsed = parse_slack_subcommand(text);
    if (!parsed) {
        // Empty / whitespace body — return help.
        std::ostringstream os;
        os << "Usage: `/hermes <command> [args]`\n";
        if (!known_.empty()) {
            os << "Commands: ";
            bool first = true;
            for (const auto& [name, _] : known_) {
                if (!first) os << ", ";
                os << name;
                first = false;
            }
        }
        return os.str();
    }

    auto it = handlers_.find(parsed->name);
    if (it != handlers_.end()) {
        return it->second(*parsed);
    }

    std::ostringstream os;
    os << "unknown command: `" << parsed->name << "`";
    if (!known_.empty()) {
        os << "\nKnown: ";
        bool first = true;
        for (const auto& [name, _] : known_) {
            if (!first) os << ", ";
            os << name;
            first = false;
        }
    }
    return os.str();
}

}  // namespace hermes::gateway
