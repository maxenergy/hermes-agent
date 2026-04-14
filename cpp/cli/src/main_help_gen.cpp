// main_help_gen — static command catalog and help rendering.  See the
// companion header for the full API surface.  The catalog is the C++
// translation of Python's hermes_cli/commands.py::COMMAND_REGISTRY
// combined with the argparse setup from hermes_cli/main.py::main().

#include "hermes/cli/main_help_gen.hpp"

#include <algorithm>
#include <cstring>
#include <sstream>

namespace hermes::cli::help_gen {

namespace {

// Quick factory for FlagDef values — keeps the registry table readable.
FlagDef F(std::string long_name,
          std::string short_name,
          std::string metavar,
          std::string help,
          std::string def = "",
          bool repeatable = false) {
    FlagDef f;
    f.long_name = std::move(long_name);
    f.short_name = std::move(short_name);
    f.metavar = std::move(metavar);
    f.help = std::move(help);
    f.default_val = std::move(def);
    f.repeatable = repeatable;
    return f;
}

CommandDef make_cmd(std::string name,
                    std::string summary,
                    std::string category,
                    std::vector<FlagDef> flags = {},
                    std::vector<CommandDef> subs = {}) {
    CommandDef c;
    c.name = std::move(name);
    c.summary = std::move(summary);
    c.category = std::move(category);
    c.flags = std::move(flags);
    c.subcommands = std::move(subs);
    return c;
}

// Build the full registry — called once and cached.
std::vector<CommandDef> build_registry() {
    std::vector<CommandDef> r;

    // -- chat ---------------------------------------------------------------
    r.push_back(make_cmd(
        "chat",
        "Interactive chat with the agent (default)",
        "chat",
        {
            F("--query", "-q", "STRING", "Single query (non-interactive mode)"),
            F("--image", "", "PATH", "Optional local image path to attach"),
            F("--model", "-m", "MODEL", "Model to use"),
            F("--toolsets", "-t", "LIST", "Comma-separated toolsets to enable"),
            F("--skills", "-s", "LIST", "Preload one or more skills", "", true),
            F("--provider", "", "ID",
              "Inference provider (auto|openrouter|nous|copilot|anthropic|…)"),
            F("--verbose", "-v", "", "Verbose output"),
            F("--quiet", "-Q", "", "Quiet mode — only final response"),
            F("--resume", "-r", "ID", "Resume a previous session by ID"),
            F("--continue", "-c", "NAME",
              "Resume a session by name, or the most recent if omitted"),
            F("--worktree", "-w", "",
              "Run in an isolated git worktree for parallel agents"),
            F("--checkpoints", "", "",
              "Enable filesystem checkpoints before destructive writes"),
            F("--max-turns", "", "N",
              "Max tool-calling iterations per turn (default: 90)"),
            F("--yolo", "", "",
              "Bypass dangerous-command approval prompts (use at your own risk)"),
            F("--pass-session-id", "", "",
              "Include the session ID in the agent's system prompt"),
            F("--source", "", "TAG",
              "Session source tag for filtering (default: cli)"),
        }));

    // -- model --------------------------------------------------------------
    r.push_back(make_cmd(
        "model",
        "Select default model and provider",
        "chat",
        {
            F("--portal-url", "", "URL", "Portal base URL for Nous login"),
            F("--inference-url", "", "URL", "Inference API base URL for Nous login"),
            F("--client-id", "", "ID", "OAuth client id (default: hermes-cli)"),
            F("--scope", "", "SCOPE", "OAuth scope to request"),
            F("--no-browser", "", "", "Do not auto-open the browser"),
            F("--timeout", "", "SECS", "HTTP request timeout (default: 15)"),
            F("--ca-bundle", "", "PATH", "Path to CA bundle PEM file"),
            F("--insecure", "", "", "Disable TLS verification (testing only)"),
        },
        {
            make_cmd("switch", "Switch model non-interactively", "chat",
                     {F("", "", "PROVIDER:MODEL", "e.g. anthropic:claude-opus-4-6")}),
        }));

    // -- gateway ------------------------------------------------------------
    r.push_back(make_cmd(
        "gateway", "Messaging gateway management", "infra", {},
        {
            make_cmd("run", "Run gateway in foreground", "infra",
                     {F("--verbose", "-v", "",
                        "Increase stderr log verbosity (-v=INFO, -vv=DEBUG)"),
                      F("--quiet", "-q", "", "Suppress stderr log output"),
                      F("--replace", "", "",
                        "Replace any existing gateway instance")}),
            make_cmd("start", "Start the installed background service", "infra",
                     {F("--system", "", "",
                        "Target the Linux system-level gateway service")}),
            make_cmd("stop", "Stop gateway service", "infra",
                     {F("--system", "", "", "Target system-level service"),
                      F("--all", "", "",
                        "Stop ALL gateway processes across all profiles")}),
            make_cmd("restart", "Restart gateway service", "infra",
                     {F("--system", "", "", "Target system-level service")}),
            make_cmd("status", "Show gateway status", "infra",
                     {F("--deep", "", "", "Deep status check"),
                      F("--system", "", "", "Target system-level service")}),
            make_cmd("install", "Install gateway as a background service", "infra",
                     {F("--force", "", "", "Force reinstall"),
                      F("--system", "", "", "Install as system service"),
                      F("--run-as-user", "", "USER",
                        "User account the system service should run as")}),
            make_cmd("uninstall", "Uninstall gateway service", "infra",
                     {F("--system", "", "", "Target system-level service")}),
            make_cmd("setup", "Configure messaging platforms", "infra"),
        }));

    // -- setup --------------------------------------------------------------
    r.push_back(make_cmd(
        "setup", "Interactive setup wizard", "onboarding",
        {
            F("", "", "SECTION",
              "Run a specific section: model|tts|terminal|gateway|tools|agent"),
            F("--non-interactive", "", "", "Use defaults/env vars"),
            F("--reset", "", "", "Reset configuration to defaults"),
        }));

    // -- whatsapp -----------------------------------------------------------
    r.push_back(make_cmd("whatsapp",
                         "Set up WhatsApp integration (QR pairing)",
                         "infra"));

    // -- login / logout / auth ---------------------------------------------
    r.push_back(make_cmd(
        "login", "Authenticate with an inference provider", "auth",
        {
            F("--provider", "", "ID", "Provider (nous|openai-codex)"),
            F("--portal-url", "", "URL", "Portal base URL"),
            F("--inference-url", "", "URL", "Inference API base URL"),
            F("--client-id", "", "ID", "OAuth client id"),
            F("--scope", "", "SCOPE", "OAuth scope to request"),
            F("--no-browser", "", "", "Do not auto-open browser"),
            F("--timeout", "", "SECS", "HTTP request timeout (default: 15)"),
            F("--ca-bundle", "", "PATH", "CA bundle PEM path"),
            F("--insecure", "", "", "Disable TLS verification"),
        }));

    r.push_back(make_cmd(
        "logout", "Clear authentication for an inference provider", "auth",
        {F("--provider", "", "ID", "Provider to log out from")}));

    r.push_back(make_cmd(
        "auth", "Manage pooled provider credentials", "auth", {},
        {
            make_cmd("add", "Add a pooled credential", "auth",
                     {F("", "", "PROVIDER", "Provider id (e.g. anthropic)"),
                      F("--type", "", "oauth|api-key", "Credential type"),
                      F("--label", "", "TEXT", "Display label"),
                      F("--api-key", "", "KEY", "API key value"),
                      F("--portal-url", "", "URL", "Nous portal URL"),
                      F("--inference-url", "", "URL", "Nous inference URL"),
                      F("--client-id", "", "ID", "OAuth client id"),
                      F("--scope", "", "SCOPE", "OAuth scope"),
                      F("--no-browser", "", "", "No browser for OAuth"),
                      F("--timeout", "", "SECS", "OAuth/network timeout"),
                      F("--insecure", "", "", "Disable TLS verification"),
                      F("--ca-bundle", "", "PATH", "Custom CA bundle")}),
            make_cmd("list", "List pooled credentials", "auth",
                     {F("", "", "PROVIDER", "Optional provider filter")}),
            make_cmd("remove", "Remove a pooled credential", "auth",
                     {F("", "", "PROVIDER", "Provider id"),
                      F("", "", "TARGET", "Credential index, id, or label")}),
            make_cmd("reset", "Clear exhaustion status for a provider", "auth",
                     {F("", "", "PROVIDER", "Provider id")}),
            make_cmd("status", "Show aggregate auth status", "auth"),
        }));

    // -- status / doctor / version -----------------------------------------
    r.push_back(make_cmd(
        "status", "Show status of all components", "debug",
        {F("--all", "", "", "Show all details (redacted for sharing)"),
         F("--deep", "", "", "Run deep checks")}));

    r.push_back(make_cmd(
        "doctor", "Check configuration and dependencies", "debug",
        {F("--verbose", "-v", "", "Verbose diagnostic output"),
         F("--json", "", "", "Emit machine-readable JSON"),
         F("--no-color", "", "", "Disable ANSI color")}));

    r.push_back(make_cmd("version", "Show version", "debug"));
    r.push_back(make_cmd("update", "Update to latest version", "infra",
                         {F("--gateway", "", "", "Forward prompts via gateway IPC"),
                          F("--force", "", "", "Reinstall even if up to date"),
                          F("--zip", "", "", "Use ZIP fallback (Windows)")}));
    r.push_back(make_cmd("uninstall", "Uninstall Hermes Agent", "infra"));

    // -- cron ---------------------------------------------------------------
    r.push_back(make_cmd(
        "cron", "Cron job management", "infra", {},
        {
            make_cmd("list", "List scheduled jobs", "infra",
                     {F("--all", "", "", "Include disabled jobs")}),
            make_cmd("create", "Create a scheduled job", "infra",
                     {F("", "", "SCHEDULE", "Schedule expression"),
                      F("", "", "PROMPT", "Optional task instruction"),
                      F("--name", "", "TEXT", "Human-friendly job name"),
                      F("--deliver", "", "TARGET", "Delivery target"),
                      F("--repeat", "", "N", "Repeat count"),
                      F("--skill", "", "ID", "Attach a skill", "", true),
                      F("--script", "", "PATH",
                        "Path to a Python script; stdout injected into prompt")}),
            make_cmd("edit", "Edit an existing scheduled job", "infra",
                     {F("", "", "JOB_ID", "Job ID"),
                      F("--schedule", "", "EXPR", "New schedule"),
                      F("--prompt", "", "TEXT", "New prompt"),
                      F("--name", "", "TEXT", "New name"),
                      F("--deliver", "", "TARGET", "New delivery target"),
                      F("--repeat", "", "N", "New repeat count"),
                      F("--skill", "", "ID", "Replace skill list", "", true),
                      F("--add-skill", "", "ID", "Append skill", "", true),
                      F("--remove-skill", "", "ID", "Remove skill", "", true),
                      F("--clear-skills", "", "", "Clear all skills"),
                      F("--script", "", "PATH", "New script path")}),
            make_cmd("pause", "Pause a scheduled job", "infra",
                     {F("", "", "JOB_ID", "Job ID")}),
            make_cmd("resume", "Resume a paused job", "infra",
                     {F("", "", "JOB_ID", "Job ID")}),
            make_cmd("run", "Trigger a job on next tick", "infra",
                     {F("", "", "JOB_ID", "Job ID")}),
            make_cmd("remove", "Remove a scheduled job", "infra",
                     {F("", "", "JOB_ID", "Job ID")}),
            make_cmd("status", "Check if cron scheduler is running", "infra"),
            make_cmd("tick", "Run due jobs once and exit", "debug"),
        }));
    // cron aliases
    r.back().subcommands[1].aliases = {"add"};     // create
    r.back().subcommands[6].aliases = {"rm", "delete"}; // remove

    // -- profile ------------------------------------------------------------
    r.push_back(make_cmd(
        "profile", "Manage user profiles", "infra", {},
        {
            make_cmd("list", "List profiles", "infra"),
            make_cmd("show", "Show profile details", "infra",
                     {F("", "", "NAME", "Profile name")}),
            make_cmd("create", "Create a new profile", "infra",
                     {F("", "", "NAME", "Profile name"),
                      F("--clone", "", "NAME", "Clone from profile"),
                      F("--clone-all", "", "", "Clone everything"),
                      F("--no-alias", "", "", "Skip creating wrapper alias"),
                      F("--interactive", "-i", "", "Interactive wizard")}),
            make_cmd("delete", "Delete a profile", "infra",
                     {F("", "", "NAME", "Profile name"),
                      F("--yes", "-y", "", "Skip confirmation")}),
            make_cmd("rename", "Rename a profile", "infra",
                     {F("", "", "OLD", "Old name"),
                      F("", "", "NEW", "New name")}),
            make_cmd("use", "Set active profile", "infra",
                     {F("", "", "NAME", "Profile name")}),
            make_cmd("export", "Export profile archive", "infra",
                     {F("", "", "NAME", "Profile name"),
                      F("", "", "OUTPUT", "Output path (.tar.gz)")}),
            make_cmd("import", "Import profile archive", "infra",
                     {F("", "", "ARCHIVE", "Archive path"),
                      F("--name", "-n", "NAME", "Renamed profile")}),
            make_cmd("alias", "Create a wrapper alias", "infra",
                     {F("", "", "NAME", "Profile name")}),
            make_cmd("completion", "Shell completion output", "infra",
                     {F("", "", "SHELL", "bash|zsh|fish")}),
        }));

    // -- sessions -----------------------------------------------------------
    r.push_back(make_cmd(
        "sessions", "Session browser / search / rename", "chat", {},
        {
            make_cmd("list", "List sessions", "chat",
                     {F("--limit", "-n", "N", "Max rows"),
                      F("--source", "", "TAG", "Filter by source tag"),
                      F("--since", "", "DURATION", "Only sessions active since"),
                      F("--json", "", "", "Emit JSON")}),
            make_cmd("browse", "Interactive session picker", "chat"),
            make_cmd("rename", "Rename / title a session", "chat",
                     {F("", "", "ID", "Session ID"),
                      F("", "", "TITLE", "New title")}),
            make_cmd("search", "Full-text search across sessions", "chat",
                     {F("", "", "QUERY", "Search query"),
                      F("--limit", "-n", "N", "Max rows")}),
            make_cmd("show", "Print a session transcript", "chat",
                     {F("", "", "ID", "Session ID"),
                      F("--format", "", "FORMAT", "text|markdown|json")}),
            make_cmd("delete", "Delete a session", "chat",
                     {F("", "", "ID", "Session ID"),
                      F("--yes", "-y", "", "Skip confirmation")}),
        }));

    // -- config -------------------------------------------------------------
    r.push_back(make_cmd(
        "config", "Configuration management", "infra", {},
        {
            make_cmd("set", "Set a configuration value", "infra",
                     {F("", "", "KEY", "Dotted key path"),
                      F("", "", "VALUE", "Value (parsed as JSON if possible)")}),
            make_cmd("get", "Get a configuration value", "infra",
                     {F("", "", "KEY", "Dotted key path")}),
            make_cmd("unset", "Remove a configuration value", "infra",
                     {F("", "", "KEY", "Dotted key path")}),
            make_cmd("edit", "Open the config file in $EDITOR", "infra"),
            make_cmd("migrate", "Re-run the config migration", "infra"),
            make_cmd("reset", "Reset to default configuration", "infra",
                     {F("--yes", "-y", "", "Skip confirmation")}),
        }));

    // -- tools / skills / models / mcp / plugins ---------------------------
    r.push_back(make_cmd("tools", "List available tools / toolsets", "chat",
                         {F("--all", "", "", "Show all tools, not just resolved set")}));
    r.push_back(make_cmd("skills", "List available skills", "chat",
                         {F("--all", "", "", "Include disabled skills")}));
    r.push_back(make_cmd("models", "Browse known models", "chat",
                         {F("--provider", "", "ID", "Filter by provider")}));
    r.push_back(make_cmd(
        "mcp", "Model Context Protocol server management", "infra", {},
        {make_cmd("list", "List registered MCP servers", "infra"),
         make_cmd("add", "Add an MCP server", "infra",
                  {F("", "", "NAME", "Server name"),
                   F("", "", "COMMAND", "Command to spawn")}),
         make_cmd("remove", "Remove an MCP server", "infra",
                  {F("", "", "NAME", "Server name")})}));
    r.push_back(make_cmd(
        "plugins", "Manage plugins", "infra", {},
        {make_cmd("list", "List installed plugins", "infra"),
         make_cmd("install", "Install a plugin", "infra",
                  {F("", "", "NAME_OR_URL", "Plugin name or URL")}),
         make_cmd("uninstall", "Uninstall a plugin", "infra",
                  {F("", "", "NAME", "Plugin name")}),
         make_cmd("enable", "Enable a plugin", "infra",
                  {F("", "", "NAME", "Plugin name")}),
         make_cmd("disable", "Disable a plugin", "infra",
                  {F("", "", "NAME", "Plugin name")}),
         make_cmd("info", "Show plugin metadata", "infra",
                  {F("", "", "NAME", "Plugin name")}),
         make_cmd("search", "Search for plugins", "infra",
                  {F("", "", "QUERY", "Search query")}),
         make_cmd("reload", "Reload plugin registry", "infra")}));

    // -- memory (Honcho) ----------------------------------------------------
    r.push_back(make_cmd(
        "memory", "Configure memory backend (Honcho)", "infra", {},
        {make_cmd("status", "Show Honcho config/connection status", "infra"),
         make_cmd("setup", "Configure Honcho integration", "infra"),
         make_cmd("sessions", "List dir → session name mappings", "infra"),
         make_cmd("map", "Map current directory to a session name", "infra",
                  {F("", "", "NAME", "Session name")}),
         make_cmd("peer", "Show/set peer names", "infra",
                  {F("--user", "", "NAME", "Set user peer name"),
                   F("--ai", "", "NAME", "Set AI peer name"),
                   F("--reasoning", "", "LEVEL", "Dialectic reasoning level")}),
         make_cmd("mode", "Show/set memory mode (hybrid|honcho|local)", "infra"),
         make_cmd("tokens", "Token budget settings", "infra",
                  {F("--context", "", "N", "session.context() cap"),
                   F("--dialectic", "", "N", "dialectic result cap")}),
         make_cmd("identity", "AI peer identity representation", "infra"),
         make_cmd("migrate", "Migration guide from OpenClaw", "infra")}));

    // -- dump / webhook / runtime / logs / providers -----------------------
    r.push_back(make_cmd(
        "dump", "Export sessions/config/memory for support", "debug",
        {F("--since", "", "DURATION", "Only last N hours/days"),
         F("--output", "", "PATH", "Output archive path"),
         F("--redact", "", "", "Redact API keys/secrets")}));
    r.push_back(make_cmd(
        "webhook", "Webhook subscription management", "infra", {},
        {make_cmd("list", "List webhook endpoints", "infra"),
         make_cmd("add", "Add webhook endpoint", "infra",
                  {F("", "", "URL", "Endpoint URL"),
                   F("--event", "", "NAME", "Event filter", "", true)}),
         make_cmd("remove", "Remove webhook endpoint", "infra",
                  {F("", "", "URL_OR_INDEX", "URL or index")})}));
    r.push_back(make_cmd(
        "runtime", "Switch terminal backend", "infra", {},
        {make_cmd("list", "List available backends", "infra"),
         make_cmd("select", "Select a backend", "infra",
                  {F("", "", "NAME", "Backend name")})}));
    r.push_back(make_cmd(
        "logs", "Show recent log output", "debug",
        {F("--follow", "-f", "", "Tail the log file"),
         F("--since", "", "DURATION", "Only lines since"),
         F("--limit", "-n", "N", "Max lines (default: 50)"),
         F("", "", "STREAM", "agent|errors|gateway (default: agent)")}));
    r.push_back(make_cmd(
        "providers", "List configured providers and key status", "auth", {},
        {make_cmd("list", "List providers", "auth"),
         make_cmd("test", "Probe a provider's reachability", "auth",
                  {F("", "", "NAME", "Provider name")})}));

    // -- claw (OpenClaw migration) -----------------------------------------
    r.push_back(make_cmd(
        "claw", "OpenClaw migration utility", "onboarding", {},
        {make_cmd("migrate", "Run migration", "onboarding",
                  {F("--dry-run", "", "", "Preview without writing"),
                   F("--overwrite", "", "", "Replace existing data"),
                   F("--preset", "", "NAME", "full|user-data|no-secrets"),
                   F("--workspace-target", "", "PATH", "Target workspace")})}));

    // -- pairing ------------------------------------------------------------
    r.push_back(make_cmd(
        "pairing", "Manage DM pairing codes", "infra", {},
        {make_cmd("approve", "Approve a pending pairing code", "infra",
                  {F("", "", "PLATFORM", "Platform name"),
                   F("", "", "CODE", "Pairing code")})}));

    // -- honcho (alias of memory for Python-compat) ------------------------
    r.push_back(make_cmd("honcho",
                         "Alias for `hermes memory` (legacy)",
                         "infra"));

    // -- rl -----------------------------------------------------------------
    r.push_back(make_cmd(
        "rl", "RL training / eval CLI", "debug", {},
        {make_cmd("train", "Run training loop", "debug"),
         make_cmd("eval", "Run evaluation", "debug"),
         make_cmd("list-environments", "List registered Atropos envs", "debug")}));

    // -- completion ---------------------------------------------------------
    r.push_back(make_cmd("completion",
                         "Emit shell completion script",
                         "infra",
                         {F("", "", "SHELL", "bash|zsh|fish")}));

    // -- acp ----------------------------------------------------------------
    r.push_back(make_cmd(
        "acp", "Run as an ACP server for editor integration", "infra",
        {F("--host", "", "HOST", "Bind host (default: 127.0.0.1)"),
         F("--port", "", "PORT", "Bind port")}));

    return r;
}

}  // namespace

// ---------------------------------------------------------------------------
// Registry access
// ---------------------------------------------------------------------------
const std::vector<CommandDef>& registry() {
    static const std::vector<CommandDef> r = build_registry();
    return r;
}

const CommandDef* find_command(const std::string& name_or_alias) {
    if (name_or_alias.empty()) return nullptr;

    // Support dotted paths — "gateway.install" means subcommand "install".
    std::string head = name_or_alias;
    std::string rest;
    auto dot = name_or_alias.find('.');
    if (dot != std::string::npos) {
        head = name_or_alias.substr(0, dot);
        rest = name_or_alias.substr(dot + 1);
    }

    for (const auto& c : registry()) {
        if (c.name == head ||
            std::find(c.aliases.begin(), c.aliases.end(), head) != c.aliases.end()) {
            if (rest.empty()) return &c;
            // Recurse into subcommands.
            for (const auto& sc : c.subcommands) {
                if (sc.name == rest ||
                    std::find(sc.aliases.begin(), sc.aliases.end(), rest) !=
                        sc.aliases.end()) {
                    return &sc;
                }
            }
            return nullptr;
        }
    }
    return nullptr;
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
const std::vector<std::string>& categories() {
    static const std::vector<std::string> cats = {
        "chat", "onboarding", "auth", "infra", "debug",
    };
    return cats;
}

std::vector<std::string> all_command_names() {
    std::vector<std::string> out;
    for (const auto& c : registry()) {
        out.push_back(c.name);
        for (const auto& a : c.aliases) out.push_back(a);
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

std::string pad_to(const std::string& s, std::size_t width) {
    if (s.size() >= width) {
        if (width <= 1) return s.substr(0, width);
        return s.substr(0, width - 1) + std::string("…");
    }
    return s + std::string(width - s.size(), ' ');
}

std::string word_wrap(const std::string& input, int width, int indent) {
    if (width <= indent + 1) return input;
    std::ostringstream out;
    std::string pad(static_cast<std::size_t>(indent), ' ');
    int col = indent;
    std::string word;
    auto flush_word = [&]() {
        if (word.empty()) return;
        int w = static_cast<int>(word.size());
        if (col > indent && col + 1 + w > width) {
            out << '\n' << pad;
            col = indent;
        } else if (col > indent) {
            out << ' ';
            ++col;
        }
        out << word;
        col += w;
        word.clear();
    };
    for (char c : input) {
        if (c == '\n') {
            flush_word();
            out << '\n' << pad;
            col = indent;
        } else if (std::isspace(static_cast<unsigned char>(c))) {
            flush_word();
        } else {
            word.push_back(c);
        }
    }
    flush_word();
    return out.str();
}

// ---------------------------------------------------------------------------
// Rendering
// ---------------------------------------------------------------------------
namespace {

const char* category_title(const std::string& cat) {
    if (cat == "chat") return "Chat & sessions";
    if (cat == "onboarding") return "Onboarding";
    if (cat == "auth") return "Authentication";
    if (cat == "infra") return "Infrastructure";
    if (cat == "debug") return "Debug & diagnostics";
    return "Other";
}

void render_command_row(std::ostringstream& out, const CommandDef& c,
                        const HelpOptions& opts, int indent = 2) {
    std::string pad(static_cast<std::size_t>(indent), ' ');
    std::string name = c.name;
    if (!c.aliases.empty()) {
        name += " (";
        for (std::size_t i = 0; i < c.aliases.size(); ++i) {
            if (i) name += ", ";
            name += c.aliases[i];
        }
        name += ")";
    }
    std::string padded = pad_to(name, 14);
    bool highlight = (opts.color && !opts.highlight_cmd.empty() &&
                      opts.highlight_cmd == c.name);
    if (highlight) out << "\033[1;36m";
    out << pad << padded;
    if (highlight) out << "\033[0m";
    if (!c.summary.empty()) {
        out << "  " << c.summary;
    }
    out << '\n';
}

}  // namespace

std::string render_global_help(const HelpOptions& opts) {
    std::ostringstream out;
    out << "Hermes — your AI agent\n\n"
        << "Usage: hermes [--profile NAME] [subcommand] [options]\n\n"
        << "Run with no arguments to enter interactive chat.\n\n";

    for (const auto& cat : categories()) {
        out << category_title(cat) << ":\n";
        bool any = false;
        for (const auto& c : registry()) {
            if (c.category != cat) continue;
            render_command_row(out, c, opts);
            any = true;
        }
        if (!any) {
            out << "  (none)\n";
        }
        out << '\n';
    }

    if (opts.show_examples) {
        out <<
            "Examples:\n"
            "  hermes                         Start interactive chat\n"
            "  hermes chat -q \"Hello\"         Single query mode\n"
            "  hermes -c                      Resume the most recent session\n"
            "  hermes -c \"my project\"         Resume a session by name\n"
            "  hermes --resume <session_id>   Resume a specific session\n"
            "  hermes setup                   Run setup wizard\n"
            "  hermes auth add <provider>     Add a pooled credential\n"
            "  hermes model                   Select default model\n"
            "  hermes config                  View configuration\n"
            "  hermes config edit             Edit config in $EDITOR\n"
            "  hermes gateway                 Run messaging gateway\n"
            "  hermes -s hermes-agent-dev     Preload a skill\n"
            "  hermes -w                      Start in isolated git worktree\n"
            "  hermes sessions list           List past sessions\n"
            "  hermes sessions browse         Interactive session picker\n"
            "  hermes logs                    View agent.log (last 50 lines)\n"
            "  hermes logs -f                 Follow agent.log in real time\n"
            "  hermes update                  Update to latest version\n"
            "\n"
            "Run 'hermes <subcommand> --help' for detailed flags.\n";
    }
    return out.str();
}

std::string render_command_help(const CommandDef& cmd,
                                const HelpOptions& opts) {
    std::ostringstream out;
    out << "Usage: hermes " << cmd.name;
    for (const auto& p : cmd.positional) {
        out << " <" << p << ">";
    }
    if (!cmd.subcommands.empty()) {
        out << " <subcommand>";
    }
    out << " [options]\n\n";

    if (!cmd.description.empty()) {
        out << word_wrap(cmd.description, opts.terminal_width) << "\n\n";
    } else if (!cmd.summary.empty()) {
        out << cmd.summary << "\n\n";
    }

    if (!cmd.subcommands.empty()) {
        out << "Subcommands:\n";
        for (const auto& sc : cmd.subcommands) {
            render_command_row(out, sc, opts);
        }
        out << '\n';
    }

    if (!cmd.flags.empty()) {
        out << "Options:\n";
        for (const auto& f : cmd.flags) {
            std::string line;
            if (!f.long_name.empty()) {
                line = f.long_name;
                if (!f.short_name.empty()) line = f.short_name + ", " + line;
                if (!f.metavar.empty()) line += " " + f.metavar;
            } else {
                line = "<" + f.metavar + ">";
            }
            out << "  " << pad_to(line, 28) << "  " << f.help;
            if (!f.default_val.empty()) {
                out << " (default: " << f.default_val << ")";
            }
            if (f.repeatable) out << " [repeatable]";
            out << '\n';
        }
    }

    return out.str();
}

std::string render_command_table(const HelpOptions& opts) {
    std::ostringstream out;
    for (const auto& c : registry()) {
        render_command_row(out, c, opts);
    }
    return out.str();
}

// ---------------------------------------------------------------------------
// Shell completion
// ---------------------------------------------------------------------------
std::string generate_bash_completion() {
    std::ostringstream out;
    out <<
        "# bash completion for hermes\n"
        "_hermes() {\n"
        "    local cur prev opts\n"
        "    COMPREPLY=()\n"
        "    cur=\"${COMP_WORDS[COMP_CWORD]}\"\n"
        "    prev=\"${COMP_WORDS[COMP_CWORD-1]}\"\n";
    out << "    local commands=\"";
    bool first = true;
    for (const auto& c : registry()) {
        if (!first) out << ' ';
        out << c.name;
        first = false;
        for (const auto& a : c.aliases) out << ' ' << a;
    }
    out << "\"\n"
        "    if [[ $COMP_CWORD -eq 1 ]]; then\n"
        "        COMPREPLY=( $(compgen -W \"${commands}\" -- \"${cur}\") )\n"
        "        return 0\n"
        "    fi\n"
        "    case \"${COMP_WORDS[1]}\" in\n";
    for (const auto& c : registry()) {
        if (c.subcommands.empty()) continue;
        out << "        " << c.name << ")\n"
            << "            if [[ $COMP_CWORD -eq 2 ]]; then\n"
            << "                COMPREPLY=( $(compgen -W \"";
        bool f = true;
        for (const auto& sc : c.subcommands) {
            if (!f) out << ' ';
            out << sc.name;
            f = false;
            for (const auto& a : sc.aliases) out << ' ' << a;
        }
        out << "\" -- \"${cur}\") )\n"
            << "                return 0\n"
            << "            fi\n"
            << "            ;;\n";
    }
    out << "    esac\n"
        "}\n"
        "complete -F _hermes hermes\n";
    return out.str();
}

std::string generate_zsh_completion() {
    std::ostringstream out;
    out << "#compdef hermes\n"
        << "_hermes() {\n"
        << "    local -a commands\n"
        << "    commands=(\n";
    for (const auto& c : registry()) {
        out << "        '" << c.name << ":" << c.summary << "'\n";
    }
    out << "    )\n"
        << "    if (( CURRENT == 2 )); then\n"
        << "        _describe 'command' commands\n"
        << "        return\n"
        << "    fi\n"
        << "    case \"$words[2]\" in\n";
    for (const auto& c : registry()) {
        if (c.subcommands.empty()) continue;
        out << "        " << c.name << ")\n"
            << "            local -a sub\n"
            << "            sub=(\n";
        for (const auto& sc : c.subcommands) {
            out << "                '" << sc.name << ":" << sc.summary << "'\n";
        }
        out << "            )\n"
            << "            _describe 'subcommand' sub\n"
            << "            ;;\n";
    }
    out << "    esac\n"
        << "}\n"
        << "_hermes \"$@\"\n";
    return out.str();
}

std::string generate_fish_completion() {
    std::ostringstream out;
    out << "# fish completion for hermes\n";
    for (const auto& c : registry()) {
        out << "complete -c hermes -n '__fish_use_subcommand' -a '"
            << c.name << "' -d '" << c.summary << "'\n";
        for (const auto& sc : c.subcommands) {
            out << "complete -c hermes -n '__fish_seen_subcommand_from "
                << c.name << "' -a '" << sc.name << "' -d '" << sc.summary << "'\n";
        }
    }
    return out.str();
}

}  // namespace hermes::cli::help_gen
