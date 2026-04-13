// Terminal tools — terminal (foreground + background), process management.
// Registered in the "terminal" toolset via register_terminal_tools().
#pragma once

#include <functional>
#include <memory>
#include <string>

#include "hermes/environments/base.hpp"

namespace hermes::tools {

void register_terminal_tools();

// Environment factory for terminal command execution.  Batch runners
// (and SWE evaluators) install a factory so each terminal invocation is
// routed through the task's isolated environment (docker / modal /
// singularity / ...).  When no factory is installed the terminal falls
// back to ``LocalEnvironment`` — the prior behaviour.
//
// The factory receives the value of ``ToolContext::extra["environment"]``
// (if set) and must return a fresh environment instance.  Returning
// nullptr causes the caller to use local.
using TerminalEnvFactory = std::function<
    std::unique_ptr<hermes::environments::BaseEnvironment>(
        const std::string& env_name)>;

/// Install a process-wide terminal environment factory.  Pass an empty
/// ``std::function`` to clear.
void set_terminal_env_factory(TerminalEnvFactory factory);

/// Resolve the current factory → produce an environment instance.
/// Never returns nullptr — falls back to LocalEnvironment.
std::unique_ptr<hermes::environments::BaseEnvironment>
resolve_terminal_env(const std::string& env_name);

}  // namespace hermes::tools
