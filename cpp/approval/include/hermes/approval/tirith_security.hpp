// Phase 12: Tirith — "hard ceiling" command deny-list.
//
// Sits *above* the 45+ DANGER_PATTERNS used by the normal approval flow:
// even in the most paranoid YOLO mode, a Tirith match is always fatal.
// Rules are regex-based and loaded from a YAML file (or the compiled
// defaults).
#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace hermes::approval {

struct TirithRule {
    std::string pattern;      // ECMAScript regex
    std::string description;
    std::string severity;     // "deny" | "warn"
};

class TirithSecurity {
public:
    TirithSecurity();  // loads default deny rules

    void add_rule(TirithRule rule);

    /// Replace the current rule set with rules parsed from a YAML file.
    /// Supports a tiny subset — a top-level ``rules:`` sequence where each
    /// entry is a mapping with ``pattern`` / ``description`` / ``severity``.
    /// Returns true on success.
    bool load_from_yaml(const std::filesystem::path& yaml_file);

    /// Returns the full list of rules that matched the command.  Empty
    /// vector ⇒ command is allowed.
    std::vector<TirithRule> scan(const std::string& command) const;

    /// Return true when at least one ``deny`` rule matched.
    bool is_denied(const std::string& command) const;

    const std::vector<TirithRule>& rules() const { return rules_; }

private:
    std::vector<TirithRule> rules_;
};

}  // namespace hermes::approval
