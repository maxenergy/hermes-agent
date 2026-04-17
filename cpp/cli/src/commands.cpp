#include "hermes/cli/commands.hpp"

#include <algorithm>
#include <mutex>

namespace hermes::cli {

// ---------------------------------------------------------------------
// Plugin command registry + derived lookup caches.
//
// The static `command_registry()` stays immutable; plugin contributions
// live here and are serialised under `g_plugins_mu`.  Cached derived
// views (flat map, by-category, gateway known set) are rebuilt by
// `rebuild_lookups()` whenever the plugin list mutates.
// ---------------------------------------------------------------------

namespace {

std::mutex& plugins_mu() {
    static std::mutex m;
    return m;
}

std::vector<CommandDef>& plugin_commands_storage() {
    static std::vector<CommandDef> v;
    return v;
}

struct DerivedCaches {
    std::map<std::string, CommandDef> flat;
    std::map<std::string, std::vector<CommandDef>> by_category;
    std::unordered_set<std::string> gateway_known;
    bool primed = false;
};

DerivedCaches& caches() {
    static DerivedCaches c;
    return c;
}

// Walk the baseline + plugin lists; populate `DerivedCaches`.  Caller
// must hold `plugins_mu()`.
void rebuild_caches_locked() {
    DerivedCaches fresh;
    auto visit = [&](const CommandDef& cmd) {
        fresh.flat[cmd.name] = cmd;
        for (const auto& alias : cmd.aliases) {
            fresh.flat[alias] = cmd;
        }
        fresh.by_category[cmd.category].push_back(cmd);
        if (!cmd.cli_only) {
            fresh.gateway_known.insert(cmd.name);
            for (const auto& alias : cmd.aliases) {
                fresh.gateway_known.insert(alias);
            }
        }
    };
    for (const auto& cmd : command_registry()) {
        visit(cmd);
    }
    for (const auto& cmd : plugin_commands_storage()) {
        visit(cmd);
    }
    fresh.primed = true;
    caches() = std::move(fresh);
}

// Ensure caches are primed the first time they are read.  Cheap fast
// path once populated.
void ensure_primed() {
    std::lock_guard<std::mutex> lock(plugins_mu());
    if (!caches().primed) {
        rebuild_caches_locked();
    }
}

}  // namespace

namespace {

std::vector<CommandDef> build_registry() {
    return {
        // ── Session ──────────────────────────────────────────────────
        {"new",        "Start a new conversation",              "Session",       {},                   "",          true,  false, ""},
        {"reset",      "Clear conversation history",            "Session",       {},                   "",          true,  false, ""},
        {"clear",      "Clear screen and start a new session",  "Session",       {},                   "",          true,  false, ""},
        {"history",    "Show conversation history",             "Session",       {},                   "",          true,  false, ""},
        {"save",       "Save the current conversation",         "Session",       {},                   "[path]",    true,  false, ""},
        {"retry",      "Retry the last message",                "Session",       {},                   "",          true,  false, ""},
        {"undo",       "Remove the last exchange",              "Session",       {},                   "",          true,  false, ""},
        {"title",      "Set or show session title",             "Session",       {},                   "[title]",   false, false, ""},
        {"branch",     "Fork a new branch from this point",     "Session",       {},                   "[name]",    true,  false, ""},
        {"rollback",   "Roll back to a previous turn",          "Session",       {"rb"},               "[n]",       true,  false, ""},
        {"stop",       "Stop the current generation",           "Session",       {},                   "",          false, false, ""},
        {"background", "Run a prompt in the background",        "Session",       {"bg"},               "<prompt>",  true,  false, ""},
        {"btw",        "Inject a side note without resending",  "Session",       {},                   "<note>",    true,  false, ""},
        {"queue",      "Queue a follow-up prompt",              "Session",       {"q"},                "<prompt>",  false, false, ""},
        {"sessions",   "List recent sessions",                  "Session",       {"ls"},               "",          true,  false, ""},
        {"continue",   "Resume the most recent session",        "Session",       {"cont"},             "",          true,  false, ""},

        // ── Configuration ────────────────────────────────────────────
        {"config",      "Show current configuration",            "Configuration", {},                   "",          true,  false, ""},
        {"model",       "Show or switch the active model",       "Configuration", {"m"},                "[model]",   false, false, ""},
        {"provider",    "Show or switch the active provider",    "Configuration", {},                   "[provider]",false, false, ""},
        {"personality", "Set agent personality",                 "Configuration", {"persona"},          "[style]",   false, false, ""},
        {"statusbar",   "Toggle the context/model status bar",   "Configuration", {"sb"},               "[on|off]",  true,  false, ""},
        {"skin",        "Show or change the display skin/theme", "Configuration", {},                   "[name]",    true,  false, ""},
        {"voice",       "Set TTS voice",                         "Configuration", {},                   "[voice]",   false, false, ""},
        {"reasoning",   "Toggle extended thinking",              "Configuration", {"think"},            "[on|off]",  false, false, ""},
        {"fast",        "Switch to the fast model preset",       "Configuration", {},                   "",          false, false, ""},
        {"yolo",        "Toggle auto-approve mode",              "Configuration", {},                   "",          false, false, ""},
        {"verbose",     "Toggle verbose output",                 "Configuration", {"v"},                "",          false, false, ""},
        {"compress",    "Compress conversation context",         "Configuration", {},                   "[feedback good|bad [note]]", true,  false, ""},
        {"ref",         "Attach a context reference (file|url)",  "Configuration", {},                   "add <path|url> | list | remove <id>", false, false, ""},

        // ── Tools & Skills ───────────────────────────────────────────
        {"skills",      "List available skills",                 "Tools & Skills", {},                  "",          false, false, ""},
        {"tools",       "List available tools",                  "Tools & Skills", {},                  "[list|disable|enable] [name...]", false, false, ""},
        {"toolsets",    "List available toolsets",               "Tools & Skills", {},                  "",          true,  false, ""},
        {"cron",        "Manage scheduled tasks",                "Tools & Skills", {},                  "[list|pause|resume|remove|run]", true,  false, ""},
        {"browser",     "Connect browser tools to live Chrome",  "Tools & Skills", {},                  "[connect|disconnect|status]", true,  false, ""},
        {"plugins",     "List installed plugins",                "Tools & Skills", {},                  "",          true,  false, ""},
        {"reload-mcp",  "Reload MCP tool servers",               "Tools & Skills", {},                  "",          true,  false, ""},

        // ── Info ─────────────────────────────────────────────────────
        {"help",       "Show help overview",                     "Info",          {"h", "?"},           "",          false, false, ""},
        {"commands",   "List all slash commands",                "Info",          {"cmds"},              "",          false, false, ""},
        {"usage",      "Show token usage for this session",      "Info",          {},                   "",          false, false, ""},
        {"insights",   "Show agent reasoning insights",          "Info",          {},                   "",          false, false, ""},
        {"status",     "Show agent and session status",          "Info",          {},                   "",          false, false, ""},
        {"profile",    "Show user profile",                      "Info",          {},                   "",          false, false, ""},
        {"platforms",  "Show connected platforms",               "Info",          {"gateway"},          "",          true,  false, ""},
        {"prompt",     "Dump the live system prompt",            "Info",          {"sys"},              "",          false, false, ""},
        {"paste",      "Attach an image from the clipboard",     "Info",          {},                   "",          true,  false, ""},
        {"image",      "Attach a local image file",              "Info",          {},                   "<path>",    true,  false, ""},

        // ── Exit ─────────────────────────────────────────────────────
        {"exit",       "Exit the CLI",                           "Exit",          {},                   "",          true,  false, ""},
        {"quit",       "Exit the CLI",                           "Exit",          {"q!"},               "",          true,  false, ""},

        // ── Gateway-only ─────────────────────────────────────────────
        {"approve",    "Approve a pending tool call",            "Gateway",       {"ok"},               "[session|always]", false, true,  ""},
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
    // Also search plugin-contributed commands.
    std::lock_guard<std::mutex> lock(plugins_mu());
    for (const auto& cmd : plugin_commands_storage()) {
        if (cmd.name == name) return cmd;
        for (const auto& alias : cmd.aliases) {
            if (alias == name) return cmd;
        }
    }
    return std::nullopt;
}

// ---------------------------------------------------------------------
// Plugin command registration API.
// ---------------------------------------------------------------------

bool register_plugin_command(CommandDef cmd) {
    if (cmd.name.empty()) return false;

    // Reject shadowing a baseline command (by canonical name).
    for (const auto& existing : command_registry()) {
        if (existing.name == cmd.name) return false;
    }

    std::lock_guard<std::mutex> lock(plugins_mu());
    for (const auto& existing : plugin_commands_storage()) {
        if (existing.name == cmd.name) return false;
    }
    plugin_commands_storage().push_back(std::move(cmd));
    rebuild_caches_locked();
    return true;
}

std::size_t unregister_plugin_command(std::string_view name) {
    std::lock_guard<std::mutex> lock(plugins_mu());
    auto& store = plugin_commands_storage();
    const auto before = store.size();
    store.erase(std::remove_if(store.begin(), store.end(),
                               [&](const CommandDef& c) { return c.name == name; }),
                store.end());
    const auto removed = before - store.size();
    if (removed > 0) {
        rebuild_caches_locked();
    }
    return removed;
}

void clear_plugin_commands() {
    std::lock_guard<std::mutex> lock(plugins_mu());
    if (plugin_commands_storage().empty()) return;
    plugin_commands_storage().clear();
    rebuild_caches_locked();
}

void rebuild_lookups() {
    std::lock_guard<std::mutex> lock(plugins_mu());
    rebuild_caches_locked();
}

const std::unordered_set<std::string>& gateway_known_commands() {
    ensure_primed();
    return caches().gateway_known;
}

namespace {

// Helper: walk baseline + plugin commands under the plugins mutex.
template <typename Fn>
void for_each_command_locked(Fn&& fn) {
    for (const auto& cmd : command_registry()) fn(cmd);
    for (const auto& cmd : plugin_commands_storage()) fn(cmd);
}

}  // namespace

std::map<std::string, CommandDef> commands_flat() {
    std::map<std::string, CommandDef> out;
    std::lock_guard<std::mutex> lock(plugins_mu());
    for_each_command_locked([&](const CommandDef& cmd) {
        out[cmd.name] = cmd;
        for (const auto& alias : cmd.aliases) {
            out[alias] = cmd;
        }
    });
    return out;
}

std::map<std::string, std::vector<CommandDef>> commands_by_category() {
    std::map<std::string, std::vector<CommandDef>> out;
    std::lock_guard<std::mutex> lock(plugins_mu());
    for_each_command_locked([&](const CommandDef& cmd) {
        out[cmd.category].push_back(cmd);
    });
    return out;
}

std::vector<std::string> gateway_help_lines() {
    std::vector<std::string> lines;
    std::lock_guard<std::mutex> lock(plugins_mu());
    for_each_command_locked([&](const CommandDef& cmd) {
        if (cmd.cli_only) return;
        std::string line = "/" + cmd.name;
        if (!cmd.args_hint.empty()) {
            line += " " + cmd.args_hint;
        }
        line += " — " + cmd.description;
        lines.push_back(std::move(line));
    });
    return lines;
}

std::vector<std::pair<std::string, std::string>> telegram_bot_commands() {
    std::vector<std::pair<std::string, std::string>> out;
    std::lock_guard<std::mutex> lock(plugins_mu());
    for_each_command_locked([&](const CommandDef& cmd) {
        if (cmd.cli_only) return;
        out.emplace_back(cmd.name, cmd.description);
    });
    return out;
}

std::map<std::string, std::string> slack_subcommand_map() {
    std::map<std::string, std::string> out;
    std::lock_guard<std::mutex> lock(plugins_mu());
    for_each_command_locked([&](const CommandDef& cmd) {
        if (cmd.cli_only) return;
        out[cmd.name] = cmd.description;
    });
    return out;
}

}  // namespace hermes::cli
