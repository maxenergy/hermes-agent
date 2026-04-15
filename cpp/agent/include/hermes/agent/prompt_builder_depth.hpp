// Depth port of agent/prompt_builder.py helpers that transform raw
// context-file text before it is injected into the system prompt.
// The existing prompt_builder.cpp handles the slot assembly; this
// depth module adds:
//
//   * scan_context_content — prompt-injection scanner for AGENTS.md
//     / CLAUDE.md / SOUL.md with invisible-unicode detection and a
//     catalogue of regex threat patterns.
//   * strip_yaml_frontmatter — remove an optional "---" delimited
//     frontmatter from the top of a markdown body.
//   * truncate_content — head/tail truncation with an in-band marker
//     pointing at the original length.
//   * skill_should_show — conditional-activation filter.
//   * build_snapshot_entry_parts — decompose a skill file path into
//     (category, skill_name).
//   * render_project_context_section — wrap body with "## name\n\n".

#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace hermes::agent::prompt_depth {

// Exposed constants mirroring the Python module (for consumers that
// want to share the same limits).
constexpr std::size_t kContextFileMaxChars = 20000;
constexpr double kContextTruncateHeadRatio = 0.7;
constexpr double kContextTruncateTailRatio = 0.2;

// Result of scan_context_content.  `blocked` is true when at least one
// threat pattern matches.  `findings` enumerates the matched pattern
// ids (or "invisible unicode U+XXXX" entries).  `output` is the
// sanitized string to inject into the prompt — either the original
// content (when no findings) or a "[BLOCKED: ...]" marker.
struct ContextScanResult {
    bool blocked = false;
    std::vector<std::string> findings;
    std::string output;
};

ContextScanResult scan_context_content(std::string_view content,
                                       std::string_view filename);

// Python: _strip_yaml_frontmatter.
std::string strip_yaml_frontmatter(std::string_view content);

// Python: _truncate_content.  max_chars defaults to kContextFileMaxChars.
std::string truncate_content(std::string_view content,
                             std::string_view filename,
                             std::size_t max_chars = kContextFileMaxChars);

// Python: _skill_should_show.  Returns true when the skill should be
// included in the index.  When both sets are empty, the function
// returns true (backward-compat with "no filter info").
struct SkillConditions {
    std::vector<std::string> fallback_for_tools;
    std::vector<std::string> fallback_for_toolsets;
    std::vector<std::string> requires_tools;
    std::vector<std::string> requires_toolsets;
};
bool skill_should_show(const SkillConditions& conds,
                       const std::optional<std::unordered_set<std::string>>& available_tools,
                       const std::optional<std::unordered_set<std::string>>& available_toolsets);

// Python: _build_snapshot_entry — extract (skill_name, category) from
// a relative skill file path.  Example:
//   "general/my-skill/SKILL.md"    → ("my-skill", "general")
//   "a/b/my-skill/SKILL.md"        → ("my-skill", "a/b")
//   "my-skill/SKILL.md"            → ("my-skill", "my-skill")
//   "SKILL.md"                     → ("SKILL.md", "general") — parent
struct SkillPathParts {
    std::string skill_name;
    std::string category;
};
SkillPathParts parse_skill_path_parts(std::string_view relative_path,
                                      std::string_view parent_dir_name);

// Python: f"## {filename}\n\n{body}" — wraps the body with a level-2
// heading section.  Used by _load_hermes_md, _load_agents_md, etc.
std::string render_project_context_section(std::string_view filename,
                                           std::string_view body);

// Model-family sniff: does the model identifier contain any of the
// tool-use-enforcement substrings?  Used by AIAgent to decide whether
// to append TOOL_USE_ENFORCEMENT_GUIDANCE to the system prompt.
bool needs_tool_use_enforcement(std::string_view model_id);

// Model-family sniff: OpenAI gpt-/codex- family.
bool is_openai_execution_family(std::string_view model_id);

// Pattern-id list used by scan_context_content.  Exposed for tests
// that want to assert a specific pattern fires.
std::vector<std::string> threat_pattern_ids();

}  // namespace hermes::agent::prompt_depth
