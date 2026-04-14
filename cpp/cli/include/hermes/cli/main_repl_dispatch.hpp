// main_repl_dispatch — slash-command dispatch table + interrupt handling.
//
// Ports:
//   - hermes_cli/commands.py::COMMAND_REGISTRY (slash command CommandDef)
//   - cli.py::HermesCLI.process_command() dispatch
//   - Ctrl-C / Ctrl-D interrupt semantics (signal handlers, terminal mode)
//
// The REPL lives in hermes_cli.cpp; this module exposes the command catalog
// and dispatch helpers so that both the CLI REPL and the messaging gateway
// share a single source of truth for /help and slash-command routing.
#pragma once

#include <functional>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace hermes::cli::repl_dispatch {

// ---------------------------------------------------------------------------
// SlashCommand — one entry per /foo command visible in the REPL.
// ---------------------------------------------------------------------------
struct SlashCommand {
    std::string name;                      // "/help"  (with leading slash)
    std::vector<std::string> aliases;      // ["/h", "/?"]
    std::string summary;                   // one-line description
    std::string category;                  // "session"|"debug"|"model"|…
    bool takes_args = false;               // true if command reads tail text
    bool gateway_visible = true;           // appears in gateway /help
    bool requires_session = false;         // fails with "no active session"
};

// Canonical registry — built once.
const std::vector<SlashCommand>& slash_registry();

// Look up by name or alias.  Returns nullptr if not found.
const SlashCommand* find_slash_command(const std::string& name_or_alias);

// Render a /help table — used by both REPL and gateway.
std::string render_slash_help(bool gateway = false, bool color = true);

// ---------------------------------------------------------------------------
// Parsing
// ---------------------------------------------------------------------------
struct ParsedCommand {
    std::string head;           // "/help"
    std::string args;           // tail text (trimmed)
    bool valid = false;         // true if head starts with '/'
};

// Parse a raw REPL line.  Leading whitespace is trimmed.  Returns a
// ParsedCommand whose .valid reflects whether the line begins with '/'.
ParsedCommand parse_command_line(const std::string& line);

// True if `line` looks like a slash command (/foo, leading '/').
bool is_slash_command(const std::string& line);

// ---------------------------------------------------------------------------
// Dispatch
// ---------------------------------------------------------------------------
using Handler = std::function<int(const ParsedCommand&)>;

// A dispatcher holds a map of command names -> handlers and routes calls.
class Dispatcher {
   public:
    void register_handler(const std::string& name, Handler fn);
    bool has(const std::string& name) const;
    std::vector<std::string> registered_commands() const;
    int dispatch(const ParsedCommand& cmd) const;  // returns -1 if not found
    int dispatch_line(const std::string& line) const;

   private:
    std::unordered_map<std::string, Handler> handlers_;
};

// Build a Dispatcher pre-populated with a default set of stub handlers
// (used mainly by tests to ensure the registry is wired end-to-end).
Dispatcher make_default_dispatcher();

// ---------------------------------------------------------------------------
// Autocomplete helpers
// ---------------------------------------------------------------------------

// Given a partial prefix like "/he", return matching command names.
std::vector<std::string> complete_slash(const std::string& prefix);

// Given a full command + partial argument, return suggestions (stubs for
// commands that know their domain: /model, /skill, /toolset, /session).
std::vector<std::string> complete_argument(const std::string& command,
                                            const std::string& partial);

// ---------------------------------------------------------------------------
// Interrupt semantics
// ---------------------------------------------------------------------------

// Install SIGINT handler that sets the interrupt flag.  Safe to call
// multiple times; only the first installation takes effect.
void install_signal_handlers();

// Restore the default handler (used on shutdown).
void restore_signal_handlers();

// True if SIGINT was recorded since the last `clear_interrupt()`.
bool interrupt_pending();

void clear_interrupt();

// Called by the REPL on Ctrl-C.  First press sets the flag; second press
// within `double_tap_ms` triggers the exit path.  Returns true if the
// second-tap threshold was crossed.
bool handle_ctrl_c(int double_tap_ms = 1500);

// Called by the REPL on Ctrl-D (EOF).  If the current input buffer is
// empty, returns true ("exit").  Otherwise returns false ("submit /
// cancel line").
bool handle_ctrl_d(bool input_buffer_empty);

// ---------------------------------------------------------------------------
// Session lifecycle hooks — called by the REPL loop.
// ---------------------------------------------------------------------------
struct ReplState {
    std::string current_session_id;
    bool yolo_mode = false;
    bool checkpoints_enabled = false;
    int turn_count = 0;
    int ctrl_c_count = 0;
};

// Summary line printed in the REPL footer.
std::string format_repl_status(const ReplState& s);

}  // namespace hermes::cli::repl_dispatch
