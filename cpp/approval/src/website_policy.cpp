#include "hermes/approval/website_policy.hpp"

#include "hermes/core/path.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <unordered_set>
#include <utility>

namespace hermes::approval {

namespace {

std::string normalize_host(std::string_view host) {
    std::string out;
    out.reserve(host.size());
    for (char c : host) {
        out.push_back(static_cast<char>(
            std::tolower(static_cast<unsigned char>(c))));
    }
    while (!out.empty() && out.back() == '.') {
        out.pop_back();
    }
    // Strip leading "www." for comparison parity with the Python version.
    if (out.rfind("www.", 0) == 0) {
        out.erase(0, 4);
    }
    return out;
}

// Parse `scheme://host[:port]/path` (best-effort) and return the host.
// Falls back to treating the input as a bare host.
std::string extract_host(std::string_view url) {
    std::string s(url);
    auto scheme_pos = s.find("://");
    if (scheme_pos != std::string::npos) {
        s.erase(0, scheme_pos + 3);
    }
    // Strip user:pass@
    auto at_pos = s.find('@');
    if (at_pos != std::string::npos && at_pos < s.find('/')) {
        s.erase(0, at_pos + 1);
    }
    // Cut at first /, ?, #
    for (char delim : {'/', '?', '#'}) {
        auto p = s.find(delim);
        if (p != std::string::npos) s.erase(p);
    }
    // Strip :port
    auto colon = s.find(':');
    if (colon != std::string::npos) s.erase(colon);
    return normalize_host(s);
}

bool match_pattern(const std::string& host, const std::string& pattern) {
    if (host.empty() || pattern.empty()) return false;

    if (pattern.size() > 2 && pattern[0] == '*' && pattern[1] == '.') {
        const std::string suffix = pattern.substr(2);
        if (host == suffix) return true;
        if (host.size() > suffix.size() &&
            host.compare(host.size() - suffix.size() - 1, suffix.size() + 1,
                         "." + suffix) == 0) {
            return true;
        }
        return false;
    }

    if (host == pattern) return true;
    if (host.size() > pattern.size() &&
        host.compare(host.size() - pattern.size() - 1, pattern.size() + 1,
                     "." + pattern) == 0) {
        return true;
    }
    return false;
}

// Baseline list used when no override feed is configured.  User-supplied
// feeds (via `$HERMES_BLOCKED_DOMAINS_FILE` or
// `<HERMES_HOME>/approval/blocked_domains.txt`) are merged on top.
const std::array<const char*, 4>& baseline_blocked_domains() {
    static const std::array<const char*, 4> table{
        "malware.test",
        "phishing.test",
        "evil.invalid",
        "exploitkit.test",
    };
    return table;
}

// Lazy-loaded, mtime-invalidated merged blocked-domain set.  The first
// call populates from the baseline + file source; subsequent calls
// re-read the file only when its mtime advances.
const std::unordered_set<std::string>& blocked_domain_set() {
    static std::mutex mu;
    static std::unordered_set<std::string> cached;
    static std::filesystem::file_time_type cached_mtime{};
    static std::filesystem::path cached_path;
    static bool primed = false;

    std::lock_guard<std::mutex> lock(mu);

    std::filesystem::path feed_path;
    if (const char* env = std::getenv("HERMES_BLOCKED_DOMAINS_FILE");
        env && *env) {
        feed_path = env;
    } else {
        try {
            feed_path = hermes::core::path::get_hermes_home() /
                        "approval" / "blocked_domains.txt";
        } catch (...) {
        }
    }

    std::error_code ec;
    std::filesystem::file_time_type mtime{};
    bool have_file = false;
    if (!feed_path.empty() &&
        std::filesystem::is_regular_file(feed_path, ec)) {
        mtime = std::filesystem::last_write_time(feed_path, ec);
        if (!ec) have_file = true;
    }

    if (primed && cached_path == feed_path &&
        cached_mtime == mtime) {
        return cached;
    }

    std::unordered_set<std::string> fresh;
    for (const char* bad : baseline_blocked_domains()) {
        fresh.insert(bad);
    }
    if (have_file) {
        std::ifstream in(feed_path);
        std::string line;
        while (std::getline(in, line)) {
            // Strip comments (# ...) and whitespace.
            auto hash = line.find('#');
            if (hash != std::string::npos) line.erase(hash);
            auto l = line.find_first_not_of(" \t\r\n");
            auto r = line.find_last_not_of(" \t\r\n");
            if (l == std::string::npos) continue;
            std::string host = line.substr(l, r - l + 1);
            std::string norm;
            norm.reserve(host.size());
            for (char c : host) {
                norm.push_back(static_cast<char>(
                    std::tolower(static_cast<unsigned char>(c))));
            }
            if (!norm.empty()) fresh.insert(std::move(norm));
        }
    }

    cached = std::move(fresh);
    cached_mtime = mtime;
    cached_path = feed_path;
    primed = true;
    return cached;
}

}  // namespace

void WebsitePolicy::add_rule(DomainRule rule) {
    rule.pattern = normalize_host(rule.pattern);
    rules_.push_back(std::move(rule));
}

void WebsitePolicy::clear() { rules_.clear(); }

bool WebsitePolicy::is_allowed(std::string_view url) const {
    return is_host_allowed(extract_host(url));
}

bool WebsitePolicy::is_host_allowed(std::string_view host) const {
    const std::string h = normalize_host(host);
    for (const auto& rule : rules_) {
        if (match_pattern(h, rule.pattern)) {
            return rule.allow;
        }
    }
    return true;  // default allow
}

void WebsitePolicy::load_from_json(const nlohmann::json& j) {
    rules_.clear();
    if (!j.contains("rules") || !j.at("rules").is_array()) return;
    for (const auto& item : j.at("rules")) {
        DomainRule rule;
        if (item.contains("pattern") && item.at("pattern").is_string()) {
            rule.pattern = normalize_host(
                item.at("pattern").get<std::string>());
        }
        if (item.contains("allow") && item.at("allow").is_boolean()) {
            rule.allow = item.at("allow").get<bool>();
        }
        if (!rule.pattern.empty()) {
            rules_.push_back(std::move(rule));
        }
    }
}

bool is_blocked_domain(std::string_view host) {
    const std::string h = normalize_host(host);
    const auto& set = blocked_domain_set();
    if (set.count(h)) return true;
    // Subdomain match: any "<...>.bad" where "bad" is in the set.
    for (const auto& bad : set) {
        if (h.size() > bad.size() &&
            h.compare(h.size() - bad.size() - 1, bad.size() + 1,
                      "." + bad) == 0) {
            return true;
        }
    }
    return false;
}

bool is_sensitive_topic_url(std::string_view url) {
    const std::string host = extract_host(url);
    static const std::array<const char*, 6> sensitive{
        "login.microsoftonline.com",
        "accounts.google.com",
        "id.apple.com",
        "login.gov",
        "online.chase.com",
        "secure.bankofamerica.com",
    };
    for (const char* s : sensitive) {
        if (host == s) return true;
    }
    return false;
}

}  // namespace hermes::approval
