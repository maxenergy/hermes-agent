// SkillsGuard — validate that a SKILL.md file is safe to load.
//
// Checks:
//   * resolved path lies under one of the approved roots (no path traversal)
//   * file size <= max_bytes (default 256 KB)
//   * file body does not contain any of the prompt-injection patterns
//     duplicated from agent::PromptBuilder::is_injection_safe
//   * skill name (parent directory) is alphanumeric + dash + underscore
//
// YAML frontmatter parsing is intentionally light: we only verify the
// top `---` fence opens and closes. Heavy parsing belongs to the skill
// loader, not the guard.
#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace hermes::approval {

struct SkillValidation {
    bool safe = false;
    std::vector<std::string> reasons;
};

SkillValidation validate_skill(
    const std::filesystem::path& skill_md_path,
    const std::vector<std::filesystem::path>& approved_roots,
    std::size_t max_bytes = 256 * 1024);

}  // namespace hermes::approval
