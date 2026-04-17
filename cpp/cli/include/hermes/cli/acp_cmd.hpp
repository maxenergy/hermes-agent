// C++17 port of hermes_cli/main.py's `acp` subcommand + acp_adapter/entry.py.
//
// Boots the ACP (Agent Client Protocol) adapter in stdio JSON-RPC mode —
// the editor host (Zed / VS Code / JetBrains) owns our stdin/stdout, so
// we read newline-delimited JSON-RPC requests from stdin, dispatch them
// through hermes::acp::AcpAdapter, and write responses to stdout.  All
// logging and diagnostics are routed to stderr so the transport stays
// clean.
//
// The adapter is wired with a real PromptHandler that bridges `prompt` /
// `session/prompt` RPCs to hermes::agent::AIAgent.  When no LLM provider
// is configured the handler returns a structured error response rather
// than `method_not_available`, so clients see a useful diagnostic.
#pragma once

#include <iosfwd>

namespace hermes::acp {
class AcpAdapter;
}

namespace hermes::cli {

// Entry point for `hermes acp` / `hermes-acp`.  Returns the desired
// process exit code (0 on clean shutdown, non-zero on fatal errors).
int cmd_acp(int argc, char** argv);

// Testable core.  Reads newline-delimited JSON-RPC requests from `in`,
// dispatches them through `adapter`, and writes JSON-RPC responses
// (envelope: `{jsonrpc:"2.0", id:<echoed>, result:<payload>}`) to `out`.
// Exits when `in` closes or when a request with method `shutdown` /
// `exit` is received.  Does not own `adapter`.
int run_acp_stdio_loop(hermes::acp::AcpAdapter& adapter,
                       std::istream& in,
                       std::ostream& out);

}  // namespace hermes::cli
