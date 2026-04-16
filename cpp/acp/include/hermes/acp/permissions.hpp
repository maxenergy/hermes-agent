// hermes::acp::PermissionMatrix — permission policy matrix for ACP sessions.
//
// Each ACP session owns a PermissionMatrix that maps operation scopes
// (FsRead/FsWrite/TerminalExec/NetFetch/MemoryWrite/SkillInvoke) to a
// default decision (Allow/Deny/AskUser), plus an ordered list of override
// rules with optional glob patterns that match against a context field
// (path/url/command).
//
// Evaluation walks the rule list in insertion order; the first matching
// rule wins.  If no rule matches, the scope's default decision is returned.
//
// Default policy (conservative):
//   FsRead       -> Allow
//   FsWrite      -> AskUser
//   TerminalExec -> AskUser
//   NetFetch     -> AskUser
//   MemoryWrite  -> Allow
//   SkillInvoke  -> AskUser
//
// JSON shape (stable keys, used by session/get_permissions and
// session/set_permissions RPCs):
//
//   {
//     "defaults": {
//       "fs_read": "allow",
//       "fs_write": "ask_user",
//       "terminal_exec": "ask_user",
//       "net_fetch": "ask_user",
//       "memory_write": "allow",
//       "skill_invoke": "ask_user"
//     },
//     "rules": [
//       {"scope": "net_fetch", "decision": "deny", "pattern": "evil.com/*"}
//     ]
//   }
//
// Thread safety: all public methods are safe for concurrent use; an
// internal mutex serializes reads and writes.  Pattern matching is a
// simple glob (`*` wildcard only — no regex, no brace expansion) to
// avoid ReDoS and surprises.
#pragma once

#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <nlohmann/json.hpp>

namespace hermes::acp {

enum class PermissionScope {
    FsRead,        // read files
    FsWrite,       // write/create/delete files
    TerminalExec,  // execute shell commands
    NetFetch,      // HTTP/HTTPS requests
    MemoryWrite,   // write to the memory store
    SkillInvoke,   // invoke a skill
};

enum class PermissionDecision {
    Allow,    // allow without prompting
    Deny,     // reject without prompting
    AskUser,  // forward to the approval flow
};

// Single rule: scope + decision + optional glob pattern.
// Empty pattern means "applies to every operation in that scope".
struct PermissionRule {
    PermissionScope scope;
    PermissionDecision decision;
    std::string pattern;
};

class PermissionMatrix {
public:
    // Conservative defaults — see file-header comment.
    PermissionMatrix();

    PermissionMatrix(const PermissionMatrix& other);
    PermissionMatrix& operator=(const PermissionMatrix& other);
    PermissionMatrix(PermissionMatrix&& other) noexcept;
    PermissionMatrix& operator=(PermissionMatrix&& other) noexcept;
    ~PermissionMatrix() = default;

    // Serialize to JSON (see file-header for the shape).
    nlohmann::json to_json() const;

    // Parse JSON produced by to_json (or an equivalent hand-written
    // object).  Missing fields fall back to conservative defaults.
    static PermissionMatrix from_json(const nlohmann::json& j);

    // Run the rule list, then fall back to the scope default.
    // `context` may contain string fields that pattern rules match
    // against:
    //   FsRead / FsWrite    -> "path"
    //   NetFetch            -> "url"
    //   TerminalExec        -> "command"
    //   MemoryWrite         -> "key"
    //   SkillInvoke         -> "skill"
    PermissionDecision evaluate(PermissionScope scope,
                                const nlohmann::json& context) const;

    // Runtime mutation helpers.
    void set_default(PermissionScope scope, PermissionDecision d);
    void add_rule(PermissionRule r);
    void clear();

private:
    // Caller does NOT hold mu_; the method acquires it internally.
    static bool glob_match(std::string_view pattern, std::string_view value);
    static const char* context_field_for(PermissionScope s);

    mutable std::mutex mu_;
    std::unordered_map<PermissionScope, PermissionDecision> defaults_;
    std::vector<PermissionRule> rules_;
};

// String <-> enum helpers used by JSON marshalling and error messages.
std::string to_string(PermissionScope s);
std::string to_string(PermissionDecision d);
PermissionScope parse_scope(std::string_view s);
PermissionDecision parse_decision(std::string_view s);

}  // namespace hermes::acp
