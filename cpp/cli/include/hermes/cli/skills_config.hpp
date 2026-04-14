// skills_config — C++ port of hermes_cli/skills_config.py.
//
// Skills can be disabled globally or per-platform:
//
//   skills:
//     disabled: [skill-a]
//     platform_disabled:
//       telegram: [skill-c]
//       cli: []
//
// This module exposes the config-layer helpers (get/save disabled lists),
// a tiny skill/category index, and the platform label map. The curses
// wizard that drives the config is kept as a thin driver in skills_config.cpp
// that delegates to these helpers.
#pragma once

#include <nlohmann/json.hpp>

#include <optional>
#include <ostream>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace hermes::cli::skills_config {

// Built-in platform → display-label map.
// Keys are the config.yaml platform identifiers; values are the strings
// rendered in the curses picker.
const std::vector<std::pair<std::string, std::string>>& platform_labels();

// Return the display label for a platform id, or "All platforms" when the
// id isn't recognised / is empty.
std::string platform_label(const std::string& platform);

// --- Disabled set ----------------------------------------------------------

// Read the disabled skill list for a platform (pass empty string for the
// global default). A missing platform_disabled entry falls back to the
// global disabled list (matches Python behaviour).
std::set<std::string> get_disabled_skills(const nlohmann::json& config,
                                          const std::string& platform = "");

// Write the disabled skill list back into a mutable config tree. The
// list is sorted alphabetically (stable ordering).
void save_disabled_skills(nlohmann::json& config,
                          const std::set<std::string>& disabled,
                          const std::string& platform = "");

// --- Skill index -----------------------------------------------------------

struct SkillInfo {
    std::string name;
    std::string description;
    std::string category;  // "uncategorized" when missing
};

// Read a skills index from disk. For the production build this delegates
// to the skill loader; here we expose a pluggable reader so tests can
// pass in-memory data.
std::vector<SkillInfo> load_skills();

// Unique sorted category list. Empty / missing categories map to
// "uncategorized".
std::vector<std::string> categories(const std::vector<SkillInfo>& skills);

// Group skills by category → [names], sorted.
std::vector<std::pair<std::string, std::vector<std::string>>>
group_by_category(const std::vector<SkillInfo>& skills);

// Given the current `disabled` set, compute a new set after the user
// toggles a single category: if `enable` is true all skills in that
// category are removed from `disabled`; otherwise they are added.
std::set<std::string> toggle_category(
    const std::vector<SkillInfo>& skills,
    const std::set<std::string>& disabled,
    const std::string& category,
    bool enable);

// Apply a "chosen" selection set (checkbox indices) from the skills UI.
// Anything NOT chosen becomes disabled.
std::set<std::string> apply_individual_toggle(
    const std::vector<SkillInfo>& skills,
    const std::set<std::size_t>& chosen);

// Build a human-readable row for the skills table used by the picker —
// "<name>  (<category>)  —  <truncated description>".
std::string format_row(const SkillInfo& skill, std::size_t desc_len = 55);

// --- Summaries -------------------------------------------------------------

// Compute the number of enabled / disabled skills given a skill set and
// a disabled list.
struct Summary {
    std::size_t enabled = 0;
    std::size_t disabled = 0;
};
Summary summarise(const std::vector<SkillInfo>& skills,
                  const std::set<std::string>& disabled);

// Render `✓ Saved: N enabled, M disabled (<platform>)` or the no-change
// message. Empty result when no output should be produced.
std::string format_save_message(const Summary& s,
                                const std::string& platform_label_);

// --- CLI entry point -------------------------------------------------------

// Entry point used by main_entry.cpp. Returns 0 on success (including the
// user cancelling the flow).
int dispatch(int argc, char** argv);

// Non-interactive helper — print the current disabled list to `out`.
int cmd_show(const nlohmann::json& config,
             const std::string& platform,
             std::ostream& out);

}  // namespace hermes::cli::skills_config
