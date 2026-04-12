// Slash-command injection — load a skill's content as a user-message
// payload (NOT system prompt, to preserve prompt cache per AGENTS.md).
#pragma once

#include <nlohmann/json.hpp>

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace hermes::skills {

// Loaded skill ready for injection into the conversation.
struct SkillPayload {
    std::string name;
    std::string content;      // SKILL.md body (frontmatter stripped)
    nlohmann::json metadata;  // parsed frontmatter
};

// Load a skill's content by name.  Searches all skills dirs for a
// matching SKILL.md, strips frontmatter, and returns the payload.
std::optional<SkillPayload> load_skill_payload(std::string_view skill_name);

// Generate a plan file path: .hermes/plans/YYYYMMDD-HHMMSS-slug.md
std::filesystem::path build_plan_path(std::string_view slug);

// Built-in skill prompts (hardcoded, not from disk).
struct BuiltinSkill {
    std::string name;
    std::string prompt;
};
const std::vector<BuiltinSkill>& builtin_skills();
// Includes at minimum: /plan, /debug, /web-research

}  // namespace hermes::skills
