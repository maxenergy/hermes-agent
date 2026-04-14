// main_help_gen — programmatic help generator + CLI command catalog.
//
// Ports the argparse-based help surface from `hermes_cli/main.py::main()`.
// Python uses argparse subparsers; we replicate the same help output using a
// static command registry driven by CommandDef entries.
//
// This module is the single source of truth for:
//   - `hermes --help` (global help)
//   - `hermes <cmd> --help` (per-subcommand help)
//   - `hermes completion bash|zsh` (generated shell completion)
//
// The Python source is `hermes_cli/commands.py::COMMAND_REGISTRY` plus the
// argparse setup in `main.py`.  This C++ version intentionally mirrors the
// same strings so the output is identical modulo the version banner.
#pragma once

#include <initializer_list>
#include <optional>
#include <string>
#include <vector>

namespace hermes::cli::help_gen {

// ---------------------------------------------------------------------------
// CommandDef — one entry per user-visible subcommand.
// ---------------------------------------------------------------------------
struct FlagDef {
    std::string long_name;   // "--verbose"  (required)
    std::string short_name;  // "-v"         (may be empty)
    std::string metavar;     // "N"          (empty = boolean flag)
    std::string help;        // one-line description
    std::string default_val; // empty = no default
    bool repeatable = false; // argparse `action="append"`
};

struct CommandDef {
    std::string name;                   // "gateway"
    std::vector<std::string> aliases;   // e.g. {"rm","delete"} for cron remove
    std::string summary;                // one-line description for `--help`
    std::string description;            // longer help (optional)
    std::vector<FlagDef> flags;         // command-specific flags
    std::vector<std::string> positional;// names of positional args
    bool requires_tty = false;          // suppressed in pipe mode
    bool is_gateway_visible = true;     // appear in `/help` via gateway?
    std::string category;               // "chat"|"auth"|"infra"|"debug"|...
    std::vector<CommandDef> subcommands;
};

// Build the canonical registry — ported from Python's COMMAND_REGISTRY.
// Idempotent; safe to call many times.
const std::vector<CommandDef>& registry();

// Find a command (or subcommand) by name/alias.  Returns nullptr if not
// found.  Supports dotted paths like "gateway.install" to reach nested
// subcommands.
const CommandDef* find_command(const std::string& name_or_alias);

// ---------------------------------------------------------------------------
// Help rendering
// ---------------------------------------------------------------------------
struct HelpOptions {
    bool color = true;
    bool show_examples = true;
    int terminal_width = 80;
    std::string highlight_cmd;   // optional — highlight in list
};

// Full global help (what `hermes --help` prints).
std::string render_global_help(const HelpOptions& opts = {});

// Per-command help (what `hermes gateway --help` prints).
std::string render_command_help(const CommandDef& cmd,
                                const HelpOptions& opts = {});

// Render just the command table (used by `/help` in the REPL and gateway).
std::string render_command_table(const HelpOptions& opts = {});

// ---------------------------------------------------------------------------
// Shell completion
// ---------------------------------------------------------------------------
std::string generate_bash_completion();
std::string generate_zsh_completion();
std::string generate_fish_completion();

// ---------------------------------------------------------------------------
// Helpers exposed for tests
// ---------------------------------------------------------------------------

// Word-wrap a string to the given width, preserving leading indent.
std::string word_wrap(const std::string& input, int width, int indent = 0);

// Pad to a column width (truncates with … if too long).
std::string pad_to(const std::string& s, std::size_t width);

// Return all categories in the order they should appear in help.
const std::vector<std::string>& categories();

// Return all command names (sorted) across all categories — useful for
// completion generators and tests.
std::vector<std::string> all_command_names();

}  // namespace hermes::cli::help_gen
