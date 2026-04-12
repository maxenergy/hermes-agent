// Central command registry — the single source of truth for all slash commands.
//
// Every CLI, gateway, and platform adapter resolves user input through
// resolve_command() and builds help / autocomplete views from the same
// registry.  Adding a new slash command means adding one entry here.
#pragma once

#include <map>
#include <optional>
#include <string>
#include <string_view>
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

}  // namespace hermes::cli
