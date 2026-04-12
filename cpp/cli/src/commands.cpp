#include "hermes/cli/commands.hpp"

#include <algorithm>

namespace hermes::cli {

namespace {

std::vector<CommandDef> build_registry() {
    return {
        // ── Session ──────────────────────────────────────────────────
        {"new",        "Start a new conversation",              "Session",       {},                   "",          true,  false, ""},
        {"reset",      "Clear conversation history",            "Session",       {},                   "",          true,  false, ""},
        {"retry",      "Retry the last message",                "Session",       {},                   "",          true,  false, ""},
        {"undo",       "Remove the last exchange",              "Session",       {},                   "",          true,  false, ""},
        {"title",      "Set or show session title",             "Session",       {},                   "[title]",   false, false, ""},
        {"branch",     "Fork a new branch from this point",     "Session",       {},                   "[name]",    true,  false, ""},
        {"rollback",   "Roll back to a previous turn",          "Session",       {"rb"},               "[n]",       true,  false, ""},
        {"stop",       "Stop the current generation",           "Session",       {},                   "",          false, false, ""},
        {"background", "Run a prompt in the background",        "Session",       {"bg"},               "<prompt>",  true,  false, ""},
        {"btw",        "Inject a side note without resending",  "Session",       {},                   "<note>",    true,  false, ""},
        {"queue",      "Queue a follow-up prompt",              "Session",       {"q"},                "<prompt>",  false, false, ""},

        // ── Configuration ────────────────────────────────────────────
        {"model",       "Show or switch the active model",       "Configuration", {"m"},                "[model]",   false, false, ""},
        {"provider",    "Show or switch the active provider",    "Configuration", {},                   "[provider]",false, false, ""},
        {"personality", "Set agent personality",                 "Configuration", {"persona"},          "[style]",   false, false, ""},
        {"voice",       "Set TTS voice",                         "Configuration", {},                   "[voice]",   false, false, ""},
        {"reasoning",   "Toggle extended thinking",              "Configuration", {"think"},            "[on|off]",  false, false, ""},
        {"fast",        "Switch to the fast model preset",       "Configuration", {},                   "",          false, false, ""},
        {"yolo",        "Toggle auto-approve mode",              "Configuration", {},                   "",          false, false, ""},
        {"verbose",     "Toggle verbose output",                 "Configuration", {"v"},                "",          false, false, ""},
        {"compress",    "Compress conversation context",         "Configuration", {},                   "",          true,  false, ""},

        // ── Tools & Skills ───────────────────────────────────────────
        {"skills",      "List available skills",                 "Tools & Skills", {},                  "",          false, false, ""},
        {"tools",       "List available tools",                  "Tools & Skills", {},                  "",          false, false, ""},
        {"reload-mcp",  "Reload MCP tool servers",               "Tools & Skills", {},                  "",          true,  false, ""},

        // ── Info ─────────────────────────────────────────────────────
        {"help",       "Show help overview",                     "Info",          {"h", "?"},           "",          false, false, ""},
        {"commands",   "List all slash commands",                "Info",          {"cmds"},              "",          false, false, ""},
        {"usage",      "Show token usage for this session",      "Info",          {},                   "",          false, false, ""},
        {"insights",   "Show agent reasoning insights",          "Info",          {},                   "",          false, false, ""},
        {"status",     "Show agent and session status",          "Info",          {},                   "",          false, false, ""},
        {"profile",    "Show user profile",                      "Info",          {},                   "",          false, false, ""},
        {"platforms",  "Show connected platforms",               "Info",          {},                   "",          false, false, ""},

        // ── Exit ─────────────────────────────────────────────────────
        {"exit",       "Exit the CLI",                           "Exit",          {},                   "",          true,  false, ""},
        {"quit",       "Exit the CLI",                           "Exit",          {"q!"},               "",          true,  false, ""},

        // ── Gateway-only ─────────────────────────────────────────────
        {"approve",    "Approve a pending tool call",            "Gateway",       {"ok"},               "",          false, true,  ""},
        {"deny",       "Deny a pending tool call",               "Gateway",       {},                   "",          false, true,  ""},
        {"sethome",    "Set gateway home channel",               "Gateway",       {},                   "",          false, true,  "gateway.enable_sethome"},
        {"resume",     "Resume a paused session",                "Gateway",       {},                   "",          false, true,  ""},
        {"restart",    "Restart the gateway process",            "Gateway",       {},                   "",          false, true,  ""},
        {"update",     "Update Hermes to the latest version",    "Gateway",       {},                   "",          false, true,  ""},
    };
}

}  // namespace

const std::vector<CommandDef>& command_registry() {
    static const auto reg = build_registry();
    return reg;
}

std::optional<CommandDef> resolve_command(std::string_view name) {
    // Strip leading slash if present.
    if (!name.empty() && name[0] == '/') {
        name.remove_prefix(1);
    }
    for (const auto& cmd : command_registry()) {
        if (cmd.name == name) return cmd;
        for (const auto& alias : cmd.aliases) {
            if (alias == name) return cmd;
        }
    }
    return std::nullopt;
}

std::map<std::string, CommandDef> commands_flat() {
    std::map<std::string, CommandDef> out;
    for (const auto& cmd : command_registry()) {
        out[cmd.name] = cmd;
        for (const auto& alias : cmd.aliases) {
            out[alias] = cmd;
        }
    }
    return out;
}

std::map<std::string, std::vector<CommandDef>> commands_by_category() {
    std::map<std::string, std::vector<CommandDef>> out;
    for (const auto& cmd : command_registry()) {
        out[cmd.category].push_back(cmd);
    }
    return out;
}

std::vector<std::string> gateway_help_lines() {
    std::vector<std::string> lines;
    for (const auto& cmd : command_registry()) {
        if (cmd.cli_only) continue;
        std::string line = "/" + cmd.name;
        if (!cmd.args_hint.empty()) {
            line += " " + cmd.args_hint;
        }
        line += " — " + cmd.description;
        lines.push_back(std::move(line));
    }
    return lines;
}

std::vector<std::pair<std::string, std::string>> telegram_bot_commands() {
    std::vector<std::pair<std::string, std::string>> out;
    for (const auto& cmd : command_registry()) {
        if (cmd.cli_only) continue;
        out.emplace_back(cmd.name, cmd.description);
    }
    return out;
}

std::map<std::string, std::string> slack_subcommand_map() {
    std::map<std::string, std::string> out;
    for (const auto& cmd : command_registry()) {
        if (cmd.cli_only) continue;
        out[cmd.name] = cmd.description;
    }
    return out;
}

}  // namespace hermes::cli
