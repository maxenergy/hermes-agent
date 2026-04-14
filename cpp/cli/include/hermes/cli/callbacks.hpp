// C++17 port of hermes_cli/callbacks.py — interactive callbacks used
// by terminal_tool / skill setup / dangerous-command approval.
//
// The Python version plumbs into prompt_toolkit's event loop; the C++
// CLI has no equivalent TUI yet, so these callbacks prompt via stdin
// with timeouts.  They're intentionally deterministic and thread-safe
// so tests can drive them by piping to stdin.
#pragma once

#include <chrono>
#include <mutex>
#include <string>
#include <vector>

namespace hermes::cli::callbacks {

struct ClarifyResult {
    std::string response;
    bool timed_out = false;
    // When choices were offered, the 0-based index of the selection.
    int choice_index = -1;
};

// Ask a clarifying question.  When `choices` is empty, the callback
// reads a freeform line.  Otherwise it prompts for a 1-based choice.
// `timeout` is applied against stdin read; 0 disables the timeout.
ClarifyResult clarify_callback(
    const std::string& question,
    const std::vector<std::string>& choices,
    std::chrono::seconds timeout = std::chrono::seconds(120));

struct SecretResult {
    bool success = true;
    bool skipped = false;
    bool validated = false;
    std::string stored_as;
    std::string reason;
    std::string message;
};

// Prompt the user for a secret (API key / token) and stash it in
// `$HERMES_HOME/.env` as `<var_name>=<value>`.  Uses getpass-style
// no-echo input on POSIX; falls back to plain stdin on Windows.
SecretResult prompt_for_secret(const std::string& var_name,
                               const std::string& prompt_text);

// Append or update an entry in the $HERMES_HOME/.env file.
bool save_env_value_secure(const std::string& key, const std::string& value);

// Read a value from the .env file. Returns empty string when missing.
std::string read_env_value(const std::string& key);

// Approval dialog for dangerous commands.  Returns one of:
//   "once" | "session" | "always" | "deny" | "view"
std::string approval_callback(const std::string& command,
                              const std::string& description,
                              std::chrono::seconds timeout =
                                  std::chrono::seconds(60));

// Thread-safe single-flight mutex for concurrent approval requests.
std::mutex& approval_mutex();

}  // namespace hermes::cli::callbacks
