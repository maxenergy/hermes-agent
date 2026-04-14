// main_repl_dispatch — REPL command registry, parser, dispatcher, and
// signal-handling helpers.  See header for the contract.

#include "hermes/cli/main_repl_dispatch.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <csignal>
#include <cstring>
#include <mutex>
#include <sstream>

namespace hermes::cli::repl_dispatch {

namespace {

SlashCommand C(std::string name, std::vector<std::string> aliases,
               std::string summary, std::string category,
               bool takes_args = false, bool gateway_visible = true,
               bool requires_session = false) {
    SlashCommand c;
    c.name = std::move(name);
    c.aliases = std::move(aliases);
    c.summary = std::move(summary);
    c.category = std::move(category);
    c.takes_args = takes_args;
    c.gateway_visible = gateway_visible;
    c.requires_session = requires_session;
    return c;
}

std::vector<SlashCommand> build_slash() {
    std::vector<SlashCommand> r;
    r.push_back(C("/help",     {"/h", "/?"},  "Show this help",           "misc"));
    r.push_back(C("/quit",     {"/exit", "/q"}, "Exit the session",       "misc", false, false));
    r.push_back(C("/clear",    {"/cls"},      "Clear the screen",         "misc", false, false));
    r.push_back(C("/reset",    {},            "Reset the conversation",   "session", false, true, true));
    r.push_back(C("/save",     {},            "Save the session",         "session", false, true, true));
    r.push_back(C("/sessions", {"/list"},     "List recent sessions",     "session"));
    r.push_back(C("/resume",   {},            "Resume a session by id",   "session", true));
    r.push_back(C("/rename",   {"/title"},    "Rename the current session","session", true, true, true));
    r.push_back(C("/search",   {},            "Full-text search",         "session", true));
    r.push_back(C("/compact",  {},            "Compress conversation",    "session", false, true, true));
    r.push_back(C("/tokens",   {},            "Show token usage",         "debug", false, true, true));
    r.push_back(C("/model",    {},            "Switch model",             "model", true));
    r.push_back(C("/provider", {},            "Switch provider",          "model", true));
    r.push_back(C("/toolset",  {"/tools"},    "Show/switch toolsets",     "tools", true));
    r.push_back(C("/skill",    {"/skills"},   "Load/list skills",         "skills", true));
    r.push_back(C("/memory",   {},            "Show memory snapshots",    "memory"));
    r.push_back(C("/cwd",      {"/pwd"},      "Show/change working dir",  "misc", true));
    r.push_back(C("/cd",       {},            "Change working directory", "misc", true));
    r.push_back(C("/yolo",     {},            "Toggle yolo mode",         "misc"));
    r.push_back(C("/checkpoint",{"/ckpt"},    "Create a checkpoint",      "misc", true));
    r.push_back(C("/rollback", {},            "Rollback to a checkpoint", "misc", true));
    r.push_back(C("/config",   {"/cfg"},      "Show/edit config",         "misc", true));
    r.push_back(C("/doctor",   {},            "Run diagnostics",          "debug"));
    r.push_back(C("/status",   {},            "Show agent status",        "debug"));
    r.push_back(C("/version",  {"/ver"},      "Show version info",        "misc"));
    r.push_back(C("/login",    {},            "Log in to a provider",     "auth", true));
    r.push_back(C("/logout",   {},            "Log out from a provider",  "auth", true));
    r.push_back(C("/auth",     {},            "Auth management",          "auth", true));
    r.push_back(C("/cron",     {},            "Cron job management",      "infra", true));
    r.push_back(C("/cost",     {"/usage"},    "Show accrued cost",        "debug"));
    r.push_back(C("/reasoning",{},            "Set reasoning effort",     "model", true));
    r.push_back(C("/image",    {},            "Attach image to next msg", "misc", true));
    r.push_back(C("/run",      {},            "Run a shell command",      "misc", true));
    r.push_back(C("/todo",     {"/t"},        "Todo list",                "misc", true));
    r.push_back(C("/plugins",  {"/plugin"},   "Manage plugins",           "infra", true));
    r.push_back(C("/gateway",  {},            "Gateway controls",         "infra", true, false));
    r.push_back(C("/dump",     {},            "Export session/state",     "debug"));
    r.push_back(C("/logs",     {},            "Show recent logs",         "debug", true));
    r.push_back(C("/mcp",      {},            "MCP server controls",      "infra", true));
    r.push_back(C("/edit",     {},            "Edit the last message",    "session", true, true, true));
    r.push_back(C("/retry",    {"/regen"},    "Regenerate last response", "session", false, true, true));
    r.push_back(C("/export",   {},            "Export current session",   "session", true, true, true));
    return r;
}

std::string trim(const std::string& s) {
    std::size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    return s.substr(a, b - a);
}

// Signal state.
std::atomic<bool> g_interrupt{false};
std::atomic<int> g_ctrl_c_count{0};
std::atomic<long long> g_last_ctrl_c_ms{0};
std::atomic<bool> g_installed{false};

extern "C" void sigint_handler(int) {
    g_interrupt.store(true, std::memory_order_relaxed);
    g_ctrl_c_count.fetch_add(1, std::memory_order_relaxed);
}

long long now_ms() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(
               steady_clock::now().time_since_epoch()).count();
}

}  // namespace

// ---------------------------------------------------------------------------
// Registry
// ---------------------------------------------------------------------------
const std::vector<SlashCommand>& slash_registry() {
    static const std::vector<SlashCommand> r = build_slash();
    return r;
}

const SlashCommand* find_slash_command(const std::string& name) {
    if (name.empty()) return nullptr;
    std::string n = name;
    if (n[0] != '/') n = "/" + n;
    for (const auto& c : slash_registry()) {
        if (c.name == n) return &c;
        for (const auto& a : c.aliases) {
            if (a == n) return &c;
        }
    }
    return nullptr;
}

std::string render_slash_help(bool gateway, bool color) {
    std::ostringstream o;
    const char* bold  = color ? "\033[1m" : "";
    const char* reset = color ? "\033[0m" : "";
    o << bold << "Slash commands:" << reset << "\n";
    for (const auto& c : slash_registry()) {
        if (gateway && !c.gateway_visible) continue;
        std::string name = c.name;
        if (!c.aliases.empty()) {
            name += " (";
            for (std::size_t i = 0; i < c.aliases.size(); ++i) {
                if (i) name += ", ";
                name += c.aliases[i];
            }
            name += ")";
        }
        o << "  " << name;
        std::size_t pad = name.size() < 28 ? (28 - name.size()) : 1;
        o << std::string(pad, ' ') << c.summary;
        if (c.takes_args) o << " [args]";
        if (c.requires_session) o << "  (session-only)";
        o << "\n";
    }
    return o.str();
}

// ---------------------------------------------------------------------------
// Parsing
// ---------------------------------------------------------------------------
ParsedCommand parse_command_line(const std::string& line) {
    ParsedCommand out;
    auto trimmed = trim(line);
    if (trimmed.empty() || trimmed[0] != '/') return out;
    auto sp = trimmed.find_first_of(" \t");
    if (sp == std::string::npos) {
        out.head = trimmed;
    } else {
        out.head = trimmed.substr(0, sp);
        out.args = trim(trimmed.substr(sp + 1));
    }
    out.valid = true;
    return out;
}

bool is_slash_command(const std::string& line) {
    return parse_command_line(line).valid;
}

// ---------------------------------------------------------------------------
// Dispatcher
// ---------------------------------------------------------------------------
void Dispatcher::register_handler(const std::string& name, Handler fn) {
    handlers_[name] = std::move(fn);
}

bool Dispatcher::has(const std::string& name) const {
    if (handlers_.count(name)) return true;
    auto* sc = find_slash_command(name);
    return sc && handlers_.count(sc->name);
}

std::vector<std::string> Dispatcher::registered_commands() const {
    std::vector<std::string> out;
    out.reserve(handlers_.size());
    for (const auto& kv : handlers_) out.push_back(kv.first);
    std::sort(out.begin(), out.end());
    return out;
}

int Dispatcher::dispatch(const ParsedCommand& cmd) const {
    if (!cmd.valid) return -1;
    // Direct match.
    auto it = handlers_.find(cmd.head);
    if (it == handlers_.end()) {
        // Try alias resolution via registry.
        auto* sc = find_slash_command(cmd.head);
        if (!sc) return -1;
        it = handlers_.find(sc->name);
        if (it == handlers_.end()) return -1;
    }
    return it->second(cmd);
}

int Dispatcher::dispatch_line(const std::string& line) const {
    return dispatch(parse_command_line(line));
}

Dispatcher make_default_dispatcher() {
    Dispatcher d;
    for (const auto& c : slash_registry()) {
        d.register_handler(c.name, [name = c.name](const ParsedCommand&) {
            (void)name;
            return 0;  // stub
        });
    }
    return d;
}

// ---------------------------------------------------------------------------
// Autocomplete
// ---------------------------------------------------------------------------
std::vector<std::string> complete_slash(const std::string& prefix) {
    std::vector<std::string> out;
    if (prefix.empty() || prefix[0] != '/') return out;
    for (const auto& c : slash_registry()) {
        if (c.name.rfind(prefix, 0) == 0) out.push_back(c.name);
        for (const auto& a : c.aliases) {
            if (a.rfind(prefix, 0) == 0) out.push_back(a);
        }
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

std::vector<std::string> complete_argument(const std::string& command,
                                            const std::string& partial) {
    std::vector<std::string> out;
    if (command == "/model" || command == "/provider") {
        static const std::vector<std::string> known = {
            "anthropic/claude-opus-4-6", "anthropic/claude-sonnet-4-20250514",
            "openai/gpt-4o", "openai/gpt-4o-mini", "openai/o3",
            "google/gemini-2.5-pro", "google/gemini-2.5-flash",
            "qwen/qwen3-coder", "deepseek/deepseek-r1",
        };
        for (const auto& k : known) {
            if (k.rfind(partial, 0) == 0) out.push_back(k);
        }
    } else if (command == "/toolset" || command == "/tools") {
        static const std::vector<std::string> known = {
            "core", "shell", "web", "code", "search", "media",
        };
        for (const auto& k : known) {
            if (k.rfind(partial, 0) == 0) out.push_back(k);
        }
    }
    return out;
}

// ---------------------------------------------------------------------------
// Signal handling
// ---------------------------------------------------------------------------
void install_signal_handlers() {
    bool expected = false;
    if (!g_installed.compare_exchange_strong(expected, true)) return;
    std::signal(SIGINT, sigint_handler);
}

void restore_signal_handlers() {
    std::signal(SIGINT, SIG_DFL);
    g_installed.store(false);
}

bool interrupt_pending() {
    return g_interrupt.load(std::memory_order_relaxed);
}

void clear_interrupt() {
    g_interrupt.store(false, std::memory_order_relaxed);
}

bool handle_ctrl_c(int double_tap_ms) {
    auto t = now_ms();
    auto prev = g_last_ctrl_c_ms.exchange(t);
    if (prev != 0 && (t - prev) <= double_tap_ms) {
        return true;
    }
    return false;
}

bool handle_ctrl_d(bool input_buffer_empty) {
    return input_buffer_empty;
}

// ---------------------------------------------------------------------------
// Status line
// ---------------------------------------------------------------------------
std::string format_repl_status(const ReplState& s) {
    std::ostringstream o;
    o << "[session=" << (s.current_session_id.empty() ? "(new)"
                                                      : s.current_session_id);
    o << ", turns=" << s.turn_count;
    if (s.yolo_mode) o << ", yolo";
    if (s.checkpoints_enabled) o << ", ckpt";
    o << "]";
    return o.str();
}

}  // namespace hermes::cli::repl_dispatch
