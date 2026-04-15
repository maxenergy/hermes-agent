// Depth port of hermes_cli/commands.py helpers.  The CLI's C++ command
// dispatcher already exists (commands.cpp); this module adds the
// pure-logic helpers that surface commands in gateway / Telegram /
// Discord menus and the shared formatting + sanitisation utilities.
//
// Python sources:
//   * _sanitize_telegram_name          → sanitize_telegram_name
//   * _clamp_command_names             → clamp_command_names
//   * _build_description               → build_description
//   * gateway_help_lines               → format_gateway_help_line
//   * _PIPE_SUBS_RE extraction         → extract_pipe_subcommands
//   * alias_note filter                → filter_alias_noise

#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

namespace hermes::cli::commands_depth {

struct CommandSpec {
    std::string name;
    std::string description;
    std::string args_hint;
    std::vector<std::string> aliases;
    bool cli_only = false;
    bool gateway_only = false;
    bool gateway_config_gate_truthy = false;
    std::string gateway_config_gate;  // empty when unused
};

// Python: _sanitize_telegram_name(raw).  Lowercase, "-" → "_",
// strip non-[a-z0-9_], collapse multiple underscores, strip leading
// and trailing underscores.  Empty output is a valid "skip" sentinel.
std::string sanitize_telegram_name(std::string_view raw);

// Python: _clamp_command_names(entries, reserved).  32-char cap with
// collision avoidance: truncate to 32; on collision replace the last
// char with a 0-9 digit to disambiguate; drop when all digit slots
// taken or the unmodified name already collides with *reserved*.
std::vector<std::pair<std::string, std::string>> clamp_command_names(
    const std::vector<std::pair<std::string, std::string>>& entries,
    const std::unordered_set<std::string>& reserved);

// Python: _build_description(cmd).  When args_hint is non-empty,
// append "(usage: /<name> <args_hint>)".
std::string build_description(const CommandSpec& cmd);

// Python: gateway_help_lines — single-line formatter.  Produces
// "`/<name>[ <args_hint>]` -- <description>[ (alias: `/<a>`, ...)]".
// Aliases whose canonical form matches the command name (hyphen ↔
// underscore) are filtered out.
std::string format_gateway_help_line(const CommandSpec& cmd);

// Python: _is_gateway_available(cmd, config_overrides) reduced to the
// pure decision (gateway_config_gate_truthy already resolved).
bool is_gateway_available(const CommandSpec& cmd);

// Python: telegram_bot_commands — sanitise name, skip when empty.
// Produces (name, description) pairs for all gateway-available
// commands in the supplied registry.
std::vector<std::pair<std::string, std::string>> telegram_bot_commands(
    const std::vector<CommandSpec>& registry);

// Python: _PIPE_SUBS_RE extraction.  Finds the first pipe-separated
// lowercase alpha run inside *args_hint*, e.g. "[on|off|tts]" →
// {"on","off","tts"}.  Returns std::nullopt when no match.
std::optional<std::vector<std::string>> extract_pipe_subcommands(
    std::string_view args_hint);

// Python: filter alias list against the hyphen↔underscore noise
// (reload_mcp vs reload-mcp).  Returns the visible aliases.
std::vector<std::string> filter_alias_noise(
    std::string_view canonical_name,
    const std::vector<std::string>& aliases);

// Constants exposed for tests.
constexpr std::size_t kCmdNameLimit = 32;

}  // namespace hermes::cli::commands_depth
