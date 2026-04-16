#include "hermes/acp/permissions.hpp"

#include <stdexcept>

namespace hermes::acp {

namespace {

// Parse with a fallback — used for to-be-lenient JSON parsing.
PermissionScope parse_scope_or(std::string_view s, PermissionScope fallback) {
    try {
        return parse_scope(s);
    } catch (const std::exception&) {
        return fallback;
    }
}

PermissionDecision parse_decision_or(std::string_view s,
                                     PermissionDecision fallback) {
    try {
        return parse_decision(s);
    } catch (const std::exception&) {
        return fallback;
    }
}

}  // namespace

std::string to_string(PermissionScope s) {
    switch (s) {
        case PermissionScope::FsRead:       return "fs_read";
        case PermissionScope::FsWrite:      return "fs_write";
        case PermissionScope::TerminalExec: return "terminal_exec";
        case PermissionScope::NetFetch:     return "net_fetch";
        case PermissionScope::MemoryWrite:  return "memory_write";
        case PermissionScope::SkillInvoke:  return "skill_invoke";
    }
    return "unknown_scope";
}

std::string to_string(PermissionDecision d) {
    switch (d) {
        case PermissionDecision::Allow:   return "allow";
        case PermissionDecision::Deny:    return "deny";
        case PermissionDecision::AskUser: return "ask_user";
    }
    return "unknown_decision";
}

PermissionScope parse_scope(std::string_view s) {
    if (s == "fs_read")       return PermissionScope::FsRead;
    if (s == "fs_write")      return PermissionScope::FsWrite;
    if (s == "terminal_exec") return PermissionScope::TerminalExec;
    if (s == "net_fetch")     return PermissionScope::NetFetch;
    if (s == "memory_write")  return PermissionScope::MemoryWrite;
    if (s == "skill_invoke")  return PermissionScope::SkillInvoke;
    throw std::invalid_argument("unknown permission scope: " +
                                std::string(s));
}

PermissionDecision parse_decision(std::string_view s) {
    if (s == "allow")    return PermissionDecision::Allow;
    if (s == "deny")     return PermissionDecision::Deny;
    if (s == "ask_user") return PermissionDecision::AskUser;
    throw std::invalid_argument("unknown permission decision: " +
                                std::string(s));
}

PermissionMatrix::PermissionMatrix() {
    // Conservative defaults — file-header documents the policy.
    defaults_[PermissionScope::FsRead]       = PermissionDecision::Allow;
    defaults_[PermissionScope::FsWrite]      = PermissionDecision::AskUser;
    defaults_[PermissionScope::TerminalExec] = PermissionDecision::AskUser;
    defaults_[PermissionScope::NetFetch]     = PermissionDecision::AskUser;
    defaults_[PermissionScope::MemoryWrite]  = PermissionDecision::Allow;
    defaults_[PermissionScope::SkillInvoke]  = PermissionDecision::AskUser;
}

PermissionMatrix::PermissionMatrix(const PermissionMatrix& other) {
    std::lock_guard<std::mutex> lk(other.mu_);
    defaults_ = other.defaults_;
    rules_    = other.rules_;
}

PermissionMatrix& PermissionMatrix::operator=(const PermissionMatrix& other) {
    if (this == &other) return *this;
    // Lock both — always in the same address order to avoid deadlock.
    std::mutex* first  = &mu_;
    std::mutex* second = &other.mu_;
    if (first > second) std::swap(first, second);
    std::lock_guard<std::mutex> lk_a(*first);
    std::lock_guard<std::mutex> lk_b(*second);
    defaults_ = other.defaults_;
    rules_    = other.rules_;
    return *this;
}

PermissionMatrix::PermissionMatrix(PermissionMatrix&& other) noexcept {
    std::lock_guard<std::mutex> lk(other.mu_);
    defaults_ = std::move(other.defaults_);
    rules_    = std::move(other.rules_);
}

PermissionMatrix& PermissionMatrix::operator=(PermissionMatrix&& other) noexcept {
    if (this == &other) return *this;
    std::mutex* first  = &mu_;
    std::mutex* second = &other.mu_;
    if (first > second) std::swap(first, second);
    std::lock_guard<std::mutex> lk_a(*first);
    std::lock_guard<std::mutex> lk_b(*second);
    defaults_ = std::move(other.defaults_);
    rules_    = std::move(other.rules_);
    return *this;
}

const char* PermissionMatrix::context_field_for(PermissionScope s) {
    switch (s) {
        case PermissionScope::FsRead:       return "path";
        case PermissionScope::FsWrite:      return "path";
        case PermissionScope::NetFetch:     return "url";
        case PermissionScope::TerminalExec: return "command";
        case PermissionScope::MemoryWrite:  return "key";
        case PermissionScope::SkillInvoke:  return "skill";
    }
    return "";
}

// Simple glob matcher:
//   '*' matches any run of characters (including empty);
//   every other character is a literal match.
// No escape sequences, no brace expansion, no character classes — this
// keeps the matcher linear in |pattern|+|value| and immune to ReDoS.
bool PermissionMatrix::glob_match(std::string_view pattern,
                                  std::string_view value) {
    const std::size_t p_len = pattern.size();
    const std::size_t v_len = value.size();

    std::size_t pi = 0;     // current pattern index
    std::size_t vi = 0;     // current value index
    std::size_t star = std::string_view::npos;  // last '*' in pattern
    std::size_t match = 0;  // value index where the last '*' matched

    while (vi < v_len) {
        if (pi < p_len && pattern[pi] == '*') {
            star  = pi++;
            match = vi;
        } else if (pi < p_len && pattern[pi] == value[vi]) {
            ++pi;
            ++vi;
        } else if (star != std::string_view::npos) {
            // Backtrack: have the last '*' absorb one more character.
            pi = star + 1;
            vi = ++match;
        } else {
            return false;
        }
    }

    // Consume any trailing '*' wildcards left in the pattern.
    while (pi < p_len && pattern[pi] == '*') ++pi;
    return pi == p_len;
}

PermissionDecision PermissionMatrix::evaluate(
    PermissionScope scope, const nlohmann::json& context) const {
    std::lock_guard<std::mutex> lk(mu_);

    const char* field = context_field_for(scope);
    std::string value;
    if (field && *field && context.is_object() && context.contains(field) &&
        context.at(field).is_string()) {
        value = context.at(field).get<std::string>();
    }

    for (const auto& rule : rules_) {
        if (rule.scope != scope) continue;
        if (rule.pattern.empty()) {
            return rule.decision;
        }
        if (!value.empty() && glob_match(rule.pattern, value)) {
            return rule.decision;
        }
    }

    auto it = defaults_.find(scope);
    if (it != defaults_.end()) return it->second;
    // Should not happen — the default-ctor populates every scope — but
    // fall back to AskUser to stay conservative if an odd move/copy got
    // us here with an empty map.
    return PermissionDecision::AskUser;
}

void PermissionMatrix::set_default(PermissionScope scope,
                                   PermissionDecision d) {
    std::lock_guard<std::mutex> lk(mu_);
    defaults_[scope] = d;
}

void PermissionMatrix::add_rule(PermissionRule r) {
    std::lock_guard<std::mutex> lk(mu_);
    rules_.push_back(std::move(r));
}

void PermissionMatrix::clear() {
    std::lock_guard<std::mutex> lk(mu_);
    rules_.clear();
}

nlohmann::json PermissionMatrix::to_json() const {
    std::lock_guard<std::mutex> lk(mu_);

    nlohmann::json defaults = nlohmann::json::object();
    // Emit in a stable order so round-trips are deterministic.
    static const PermissionScope kOrder[] = {
        PermissionScope::FsRead,
        PermissionScope::FsWrite,
        PermissionScope::TerminalExec,
        PermissionScope::NetFetch,
        PermissionScope::MemoryWrite,
        PermissionScope::SkillInvoke,
    };
    for (auto s : kOrder) {
        auto it = defaults_.find(s);
        if (it != defaults_.end()) {
            defaults[to_string(s)] = to_string(it->second);
        }
    }

    nlohmann::json rules = nlohmann::json::array();
    for (const auto& rule : rules_) {
        nlohmann::json entry = {
            {"scope", to_string(rule.scope)},
            {"decision", to_string(rule.decision)},
        };
        if (!rule.pattern.empty()) entry["pattern"] = rule.pattern;
        rules.push_back(std::move(entry));
    }

    return nlohmann::json{{"defaults", std::move(defaults)},
                          {"rules", std::move(rules)}};
}

PermissionMatrix PermissionMatrix::from_json(const nlohmann::json& j) {
    PermissionMatrix m;  // start from conservative defaults

    if (!j.is_object()) return m;

    if (j.contains("defaults") && j.at("defaults").is_object()) {
        for (auto it = j.at("defaults").begin(); it != j.at("defaults").end();
             ++it) {
            if (!it.value().is_string()) continue;
            try {
                PermissionScope s    = parse_scope(it.key());
                PermissionDecision d =
                    parse_decision_or(it.value().get<std::string>(),
                                      PermissionDecision::AskUser);
                m.set_default(s, d);
            } catch (const std::exception&) {
                // Skip unknown scopes silently — defensive against
                // future schema drift.
            }
        }
    }

    if (j.contains("rules") && j.at("rules").is_array()) {
        for (const auto& entry : j.at("rules")) {
            if (!entry.is_object()) continue;
            if (!entry.contains("scope") ||
                !entry.contains("decision")) {
                continue;
            }
            PermissionRule r;
            r.scope = parse_scope_or(
                entry.value("scope", std::string{}),
                PermissionScope::FsRead);
            r.decision = parse_decision_or(
                entry.value("decision", std::string{}),
                PermissionDecision::AskUser);
            r.pattern = entry.value("pattern", std::string{});
            // Only push if the scope/decision actually parsed — else the
            // rule is a no-op that would trap evaluate() with the
            // fallback scope.
            try {
                PermissionScope verify_scope =
                    parse_scope(entry.value("scope", std::string{}));
                PermissionDecision verify_dec =
                    parse_decision(entry.value("decision", std::string{}));
                (void)verify_scope;
                (void)verify_dec;
            } catch (const std::exception&) {
                continue;
            }
            m.add_rule(std::move(r));
        }
    }

    return m;
}

}  // namespace hermes::acp
