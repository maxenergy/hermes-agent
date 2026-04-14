// Code execution tool — run Python or Bash code snippets in a
// sandboxed temp file.  Port of tools/code_execution_tool.py.
//
// The C++ port mirrors the Python module's public surface: the main
// execute_code handler, the hermes_tools shim generator, a small set
// of shared helpers (shell_quote, truncate_output, JSON parse with
// tolerance), and the set of "sandbox allowed" tool names.
#pragma once

#include "hermes/tools/registry.hpp"

#include <nlohmann/json.hpp>

#include <cstddef>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace hermes::tools::code_execution {

// Maximum raw output bytes returned for stdout / stderr before we
// truncate and append the "(truncated …)" suffix.
constexpr std::size_t kMaxOutputBytes = 50 * 1024;  // 50 KB
constexpr int kDefaultTimeoutSeconds = 300;
constexpr int kMinTimeoutSeconds = 1;
constexpr int kMaxTimeoutSeconds = 3600;

// Names of tools the sandbox explicitly refuses to expose to Python
// code — every call must be dispatched via the agent, not directly
// from sandbox code.  Matches SANDBOX_ALLOWED_TOOLS in the Python.
const std::unordered_set<std::string>& sandbox_allowed_tools();

// Truncate |s| to kMaxOutputBytes and append a marker when it had to
// be shortened.
std::string truncate_output(std::string_view s,
                            std::size_t max_bytes = kMaxOutputBytes);

// Shell-quote |s| so it can be safely interpolated into a bash
// command.  Mirrors shlex.quote semantics: wrap in single quotes and
// escape embedded single quotes.
std::string shell_quote(std::string_view s);

// Emit a random-ish hex UUID.
std::string generate_uuid();

// Produce the contents of the hermes_tools.py shim used inside the
// sandbox.  |enabled_tools| restricts which stubs to emit; |transport|
// selects between "uds" and "file" transports.
std::string generate_hermes_tools_module(
    const std::vector<std::string>& enabled_tools,
    std::string_view transport = "uds");

// Build the JSON schema for the execute_code tool.  When
// |enabled_sandbox_tools| is non-empty the schema description enumerates
// the tools callable from the sandbox.
nlohmann::json build_execute_code_schema(
    const std::vector<std::string>& enabled_sandbox_tools = {});

// Return true when the environment satisfies the runtime requirements
// (python3 available on PATH).
bool check_sandbox_requirements();

// Clamp |requested| to the allowed [min, max] range.
int clamp_timeout(int requested);

// Register the execute_code tool.
void register_code_execution_tools(hermes::tools::ToolRegistry& registry);

}  // namespace hermes::tools::code_execution

namespace hermes::tools {

inline void register_code_execution_tools(ToolRegistry& registry) {
    code_execution::register_code_execution_tools(registry);
}

}  // namespace hermes::tools
