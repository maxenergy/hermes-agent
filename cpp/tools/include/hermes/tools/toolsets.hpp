// Named toolsets — groupings of individual tool names that can be enabled
// together (and composed via includes).  Mirrors ``toolsets.py``.
#pragma once

#include <map>
#include <string>
#include <vector>

namespace hermes::tools {

struct ToolsetDef {
    std::string name;
    std::string description;
    std::vector<std::string> tools;     // direct tool names
    std::vector<std::string> includes;  // other toolset names to union in
};

// Canonical list of "core" tools considered if no toolset filter is
// applied — corresponds to Python's _HERMES_CORE_TOOLS.
const std::vector<std::string>& hermes_core_tools();

// Static map of named toolsets — corresponds to Python's TOOLSETS dict.
const std::map<std::string, ToolsetDef>& toolsets();

// Recursively flatten a toolset to its full tool list.  Throws
// std::invalid_argument when ``name`` is unknown or when a circular
// include is detected.  Diamond dependencies are handled correctly.
std::vector<std::string> resolve_toolset(const std::string& name);

// Same as resolve_toolset() but accepts a list of toolset names — the
// returned vector contains the union (deduplicated, sorted).
std::vector<std::string> resolve_multiple_toolsets(
    const std::vector<std::string>& names);

// Validate that ``name`` is a known toolset.  Returns the description on
// success and throws std::invalid_argument on unknown names.
std::string validate_toolset(const std::string& name);

// Testing entry point: resolve ``name`` against an arbitrary toolset
// table.  Used by unit tests to construct cyclic / pathological tables
// without mutating the global static one.  Throws std::invalid_argument
// on unknown names or circular includes.
std::vector<std::string> resolve_toolset_in_table(
    const std::string& name,
    const std::map<std::string, ToolsetDef>& table);

}  // namespace hermes::tools
