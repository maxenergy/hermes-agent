// Central command registry — the single source of truth for all slash commands.
//
// Every CLI, gateway, and platform adapter resolves user input through
// resolve_command() and builds help / autocomplete views from the same
// registry.  Adding a new slash command means adding one entry here.
#pragma once

#include <cstddef>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

namespace hermes::cli {

struct CommandDef {
    std::string name;           // canonical name without slash
    std::string description;
    std::string category;       // Session|Configuration|Tools & Skills|Info|Exit
    std::vector<std::string> aliases;
    std::string args_hint;      // "[arg]", "<prompt>", etc.
    bool cli_only = false;
    bool gateway_only = false;
    std::string gateway_config_gate;  // config dotpath for conditional gateway visibility
};

// The single source of truth for all slash commands.
const std::vector<CommandDef>& command_registry();

// Resolve a command name or alias to its CommandDef.  Returns nullopt for
// unknown commands.
std::optional<CommandDef> resolve_command(std::string_view name);

// Derived views (all computed from command_registry).

// Flat map: every canonical name AND alias maps to the owning CommandDef.
std::map<std::string, CommandDef> commands_flat();

// Grouped by category for /help display.
std::map<std::string, std::vector<CommandDef>> commands_by_category();

// One-line help strings for gateway /help responses.
std::vector<std::string> gateway_help_lines();

// (command, description) pairs for Telegram BotFather setMyCommands — excludes
// cli_only entries.
std::vector<std::pair<std::string, std::string>> telegram_bot_commands();

// Slack subcommand map: canonical name → description for commands visible to
// Slack bot dispatch.
std::map<std::string, std::string> slack_subcommand_map();

// =====================================================================
// Dynamic command registration (for plugins).
//
// The static `command_registry()` is the canonical baseline.  Plugins
// may append additional `CommandDef`s at runtime; those extras live in
// a separate mutable vector so the baseline stays a pure-function view.
// `resolve_command()` transparently searches both.
//
// Call `rebuild_lookups()` after any mutation so cached derived views
// (`COMMANDS` flat map, `COMMANDS_BY_CATEGORY`, gateway/telegram/slack
// exports, `GATEWAY_KNOWN_COMMANDS`) reflect the new state.
// =====================================================================

// Register a plugin-contributed command.  Duplicate canonical names are
// rejected (returns false); name shadowing the static registry is also
// rejected.  On success returns true; callers may then call
// `rebuild_lookups()` to refresh the derived caches.
bool register_plugin_command(CommandDef cmd);

// Remove every plugin-contributed command whose canonical name matches
// @p name.  Returns the number of entries removed (0 or 1 in practice).
std::size_t unregister_plugin_command(std::string_view name);

// Drop every plugin-contributed command.  Intended for test teardown
// and for `PluginManager::unload_all`.
void clear_plugin_commands();

// Rebuild the cached lookup maps that downstream code reads.  Must be
// called after any mutation of the plugin command list (or when a
// plugin adds/removes entries via `register_plugin_command()`).
// Thread-safety: callers must serialise registration + rebuild; the
// lookup getters below are safe to read between rebuilds.
void rebuild_lookups();

// ----- Cached derived views (populated by `rebuild_lookups()`) -----

// The set of canonical + alias names visible to the messaging gateway.
// Derived from `command_registry()` ∪ plugin commands, excluding
// `cli_only` entries.  Matches Python's GATEWAY_KNOWN_COMMANDS frozenset.
const std::unordered_set<std::string>& gateway_known_commands();

}  // namespace hermes::cli
