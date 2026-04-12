// WebsitePolicy — domain allow/block rules for URL-capable tools.
//
// Ported from tools/website_policy.py. Default semantics: ALLOW unless a
// matching block rule fires. Wildcards: a pattern beginning with `*.`
// matches the bare domain plus any subdomain. Otherwise an exact host
// match (or `host.endswith('.' + pattern)`) is used.
//
// Rules are evaluated in insertion order; the first matching rule wins.
#pragma once

#include <nlohmann/json_fwd.hpp>

#include <string>
#include <string_view>
#include <vector>

namespace hermes::approval {

struct DomainRule {
    std::string pattern;  // e.g. "*.example.com" or "example.com"
    bool allow = false;   // true = allow, false = block
};

class WebsitePolicy {
public:
    void add_rule(DomainRule rule);
    void clear();

    bool is_allowed(std::string_view url) const;
    bool is_host_allowed(std::string_view host) const;

    // Load from a JSON object of the form:
    //   { "rules": [ { "pattern": "...", "allow": false }, ... ] }
    void load_from_json(const nlohmann::json& j);

    std::size_t rule_count() const noexcept { return rules_.size(); }

private:
    std::vector<DomainRule> rules_;
};

// Quick checks against built-in lists.
bool is_blocked_domain(std::string_view host);
bool is_sensitive_topic_url(std::string_view url);

}  // namespace hermes::approval
