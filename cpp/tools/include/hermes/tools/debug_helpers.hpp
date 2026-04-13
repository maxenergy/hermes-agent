// Phase 12: Debug logging for tool dispatch.
//
// Gated on the HERMES_DEBUG_TOOLS=1 environment variable (or an explicit
// enable_tool_call_logging(true) call in tests).  When off, all helpers are
// cheap no-ops — they do not allocate, do not serialise args, and do not
// format log lines.
#pragma once

#include <chrono>
#include <string>

#include <nlohmann/json.hpp>

namespace hermes::tools::debug {

// Toggle the tool-call logger on/off.  The constructor of the process-wide
// registry consults HERMES_DEBUG_TOOLS at first use; this setter lets tests
// force-enable or force-disable.
void enable_tool_call_logging(bool on);
bool tool_call_logging_enabled();

// Log a single tool dispatch.  ``result_preview`` should already be
// truncated by the caller (first ~500 chars).
void log_tool_call(const std::string& tool_name,
                   const nlohmann::json& args,
                   const std::string& result_preview,
                   std::chrono::milliseconds duration);

// Dump the current ToolRegistry state (names, toolsets, required env) as a
// human-readable string — useful from a debugger or a REPL ``:tools`` hook.
std::string dump_registry_state();

}  // namespace hermes::tools::debug
