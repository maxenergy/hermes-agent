// Slash-command helpers shared between CLI and gateway.
//
// Partial C++17 port of agent/skill_commands.py. Full invocation lives
// in cpp/skills, but the small pure helpers (slug, plan path, name
// sanitisation) live here so they can be unit-tested without dragging
// in the skills registry.
#pragma once

#include <chrono>
#include <filesystem>
#include <string>

namespace hermes::agent::skill_commands {

// Return the default workspace-relative markdown path for a /plan
// invocation, e.g. ".hermes/plans/2026-04-13_164512-fix-the-build.md".
// Relative paths are intentional so file-backend tools (docker, ssh,
// modal) can resolve them against the active working directory.
std::filesystem::path build_plan_path(
    const std::string& user_instruction = "",
    std::chrono::system_clock::time_point now = {});

// Turn a human heading into a short hyphenated slug. Lower-cased,
// non-alphanumerics collapsed to single '-', capped at the first 8
// words and 48 chars. Empty / all-punctuation inputs return
// "conversation-plan".
std::string make_plan_slug(const std::string& heading);

// Sanitise a skill name into a clean hyphen-separated slug suitable
// for a slash command. Lower-cased; non-[a-z0-9-] collapsed to '-';
// runs of '-' shortened to a single '-'; leading/trailing '-' trimmed.
std::string sanitise_skill_slug(const std::string& name);

}  // namespace hermes::agent::skill_commands
