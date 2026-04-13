#include "hermes/approval/tirith_security.hpp"

#include <fstream>
#include <regex>
#include <sstream>
#include <string>
#include <utility>

namespace hermes::approval {

namespace {

// Hard-coded "second layer" deny rules.  These are patterns even
// paranoid-YOLO-mode users can never bypass.  Matched case-sensitively
// against the raw command string using ECMAScript regex.
std::vector<TirithRule> default_rules() {
    return {
        {R"(\brm\s+(-[rRfFvI]*\s+)*\/(\s|$))",
         "rm -rf of root filesystem",
         "deny"},
        {R"(\brm\s+(-[rRfFvI]*\s+)*\/\*)",
         "rm of every entry under root",
         "deny"},
        {R"(\bmkfs(\.|\s))",
         "filesystem format (mkfs.*)",
         "deny"},
        {R"(\bdd\s+.*of=\/dev\/(sd|nvme|xvd|mmcblk))",
         "dd write to raw block device",
         "deny"},
        {R"(:\(\)\s*\{\s*:\s*\|\s*:\s*&\s*\}\s*;\s*:)",
         "fork-bomb (:(){:|:&};:)",
         "deny"},
        {R"(\bchmod\s+(-R\s+)?0*777\s+\/(\s|$))",
         "chmod 777 /",
         "deny"},
        {R"(\bchown\s+(-R\s+)?.*\s+\/(\s|$))",
         "chown of root filesystem",
         "deny"},
        {R"(>\s*\/dev\/(sd|nvme|xvd|mmcblk))",
         "shell redirect to raw block device",
         "deny"},
        {R"(\bshred\s+.*\/dev\/(sd|nvme|xvd|mmcblk))",
         "shred of raw block device",
         "deny"},
        {R"(\bcurl\s+[^|]*\|\s*(sudo\s+)?(sh|bash|zsh)\b)",
         "curl | sh remote execution",
         "warn"},
    };
}

// Trim surrounding whitespace, and strip matching surrounding quotes.
std::string trim_and_unquote(std::string s) {
    auto l = s.find_first_not_of(" \t\r\n");
    auto r = s.find_last_not_of(" \t\r\n");
    if (l == std::string::npos) return {};
    s = s.substr(l, r - l + 1);
    if (s.size() >= 2 &&
        ((s.front() == '"' && s.back() == '"') ||
         (s.front() == '\'' && s.back() == '\''))) {
        s = s.substr(1, s.size() - 2);
    }
    return s;
}

}  // namespace

TirithSecurity::TirithSecurity() : rules_(default_rules()) {}

void TirithSecurity::add_rule(TirithRule rule) {
    rules_.push_back(std::move(rule));
}

bool TirithSecurity::load_from_yaml(const std::filesystem::path& yaml_file) {
    std::ifstream ifs(yaml_file);
    if (!ifs) return false;

    std::vector<TirithRule> parsed;
    std::string line;
    TirithRule cur;
    bool in_list = false;
    bool in_entry = false;
    auto push_current = [&]() {
        if (in_entry && !cur.pattern.empty()) {
            parsed.push_back(std::move(cur));
        }
        cur = TirithRule{};
        in_entry = false;
    };
    while (std::getline(ifs, line)) {
        // Strip comments.
        auto hash = line.find('#');
        if (hash != std::string::npos) line.erase(hash);

        // Detect 'rules:' header.
        auto nws = line.find_first_not_of(" \t");
        if (nws == std::string::npos) continue;
        std::string trimmed = line.substr(nws);

        if (!in_list) {
            if (trimmed.rfind("rules:", 0) == 0) in_list = true;
            continue;
        }

        // New list entry starts with "- ".
        if (trimmed.rfind("- ", 0) == 0) {
            push_current();
            in_entry = true;
            trimmed = trimmed.substr(2);
            // Inline "- pattern: ..." case.
            auto colon = trimmed.find(':');
            if (colon != std::string::npos) {
                auto key = trimmed.substr(0, colon);
                auto val = trim_and_unquote(trimmed.substr(colon + 1));
                if (key == "pattern") cur.pattern = val;
                else if (key == "description") cur.description = val;
                else if (key == "severity") cur.severity = val;
            }
            continue;
        }

        if (!in_entry) continue;
        auto colon = trimmed.find(':');
        if (colon == std::string::npos) continue;
        auto key = trimmed.substr(0, colon);
        auto val = trim_and_unquote(trimmed.substr(colon + 1));
        if (key == "pattern") cur.pattern = val;
        else if (key == "description") cur.description = val;
        else if (key == "severity") cur.severity = val;
    }
    push_current();

    if (parsed.empty()) return false;
    rules_ = std::move(parsed);
    return true;
}

std::vector<TirithRule> TirithSecurity::scan(const std::string& command) const {
    std::vector<TirithRule> hits;
    for (const auto& r : rules_) {
        try {
            std::regex re(r.pattern, std::regex::ECMAScript);
            if (std::regex_search(command, re)) {
                hits.push_back(r);
            }
        } catch (const std::regex_error&) {
            // Malformed pattern — skip rather than throwing.
        }
    }
    return hits;
}

bool TirithSecurity::is_denied(const std::string& command) const {
    for (const auto& r : scan(command)) {
        if (r.severity == "deny") return true;
    }
    return false;
}

}  // namespace hermes::approval
