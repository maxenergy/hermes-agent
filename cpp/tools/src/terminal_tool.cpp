#include "hermes/tools/terminal_tool.hpp"
#include "hermes/tools/registry.hpp"
#include "hermes/environments/local.hpp"
#include "hermes/state/process_registry.hpp"
#include "hermes/core/ansi_strip.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <memory>
#include <mutex>
#include <regex>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

namespace hermes::tools {

using json = nlohmann::json;
namespace fs = std::filesystem;

namespace {

// Module-level process registry for background processes.
std::mutex g_proc_mu;
std::unique_ptr<hermes::state::ProcessRegistry> g_proc_reg;

// Process-wide terminal environment factory — set by the batch
// runner / RL CLI so terminal commands issued mid-task route through
// an isolated backend (docker / modal / ...).  Default is empty → use
// LocalEnvironment.
std::mutex g_env_factory_mu;
TerminalEnvFactory g_env_factory;

std::string env_name_from_ctx(const ToolContext& ctx) {
    if (ctx.extra.is_object() && ctx.extra.contains("environment") &&
        ctx.extra["environment"].is_string()) {
        return ctx.extra["environment"].get<std::string>();
    }
    return "local";
}

hermes::state::ProcessRegistry& proc_registry() {
    std::lock_guard<std::mutex> lk(g_proc_mu);
    if (!g_proc_reg) {
        g_proc_reg = std::make_unique<hermes::state::ProcessRegistry>();
    }
    return *g_proc_reg;
}

// Generate a simple process ID.
std::string make_process_id() {
    static std::atomic<int> counter{0};
    auto now = std::chrono::system_clock::now().time_since_epoch();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
    return "proc-" + std::to_string(ms) + "-" + std::to_string(counter++);
}

}  // namespace

void set_terminal_env_factory(TerminalEnvFactory factory) {
    std::lock_guard<std::mutex> lk(g_env_factory_mu);
    g_env_factory = std::move(factory);
}

std::unique_ptr<hermes::environments::BaseEnvironment>
resolve_terminal_env(const std::string& env_name) {
    TerminalEnvFactory factory;
    {
        std::lock_guard<std::mutex> lk(g_env_factory_mu);
        factory = g_env_factory;
    }
    if (factory) {
        auto e = factory(env_name);
        if (e) return e;
    }
    return std::make_unique<hermes::environments::LocalEnvironment>();
}

void register_terminal_tools() {
    auto& reg = ToolRegistry::instance();

    // -- terminal ----------------------------------------------------------
    {
        json schema = {
            {"type", "object"},
            {"properties",
             {{"command",
               {{"type", "string"}, {"description", "Shell command to execute"}}},
              {"timeout",
               {{"type", "integer"},
                {"description", "Timeout in seconds (default 120, max 600)"}}},
              {"background",
               {{"type", "boolean"},
                {"description", "Run in background (default false)"}}},
              {"watch_patterns",
               {{"type", "array"},
                {"items", {{"type", "string"}}},
                {"description",
                 "Patterns to watch in output (background only)"}}}}},
            {"required", json::array({"command"})}};

        ToolEntry entry;
        entry.name = "terminal";
        entry.toolset = "terminal";
        entry.schema = std::move(schema);
        entry.description = "Execute a shell command";
        entry.emoji = "\xF0\x9F\x92\xBB";  // laptop
        entry.max_result_size_chars = 100 * 1024;
        entry.handler = [](const json& args, const ToolContext& ctx) -> std::string {
            std::string command = args.at("command").get<std::string>();
            int timeout = args.value("timeout", 120);
            timeout = std::max(1, std::min(timeout, 600));
            bool background = args.value("background", false);

            std::vector<std::string> watch_patterns;
            if (args.contains("watch_patterns") && args["watch_patterns"].is_array()) {
                for (const auto& p : args["watch_patterns"]) {
                    if (p.is_string()) watch_patterns.push_back(p.get<std::string>());
                }
            }

            if (background) {
                // Create a ProcessSession and run in a background thread.
                hermes::state::ProcessSession session;
                session.id = make_process_id();
                session.command = command;
                session.task_id = ctx.task_id;
                session.session_key = ctx.session_key;
                session.cwd = ctx.cwd.empty() ? fs::current_path() : fs::path(ctx.cwd);
                session.watch_patterns = std::move(watch_patterns);
                session.started_at = std::chrono::system_clock::now();
                session.state = hermes::state::ProcessState::Running;

                std::string proc_id = session.id;
                auto& preg = proc_registry();
                preg.register_process(std::move(session));

                // Spawn background thread.
                std::string cwd = ctx.cwd;
                std::string env_name = env_name_from_ctx(ctx);
                std::thread([proc_id, command, timeout, cwd, env_name]() {
                    auto env = resolve_terminal_env(env_name);
                    hermes::environments::ExecuteOptions opts;
                    opts.timeout = std::chrono::seconds(timeout);
                    if (!cwd.empty()) opts.cwd = cwd;

                    auto result = env->execute(command, opts);

                    auto& preg = proc_registry();
                    preg.feed_output(proc_id,
                                     result.stdout_text + result.stderr_text);
                    preg.mark_exited(proc_id, result.exit_code);
                }).detach();

                return tool_result({{"process_id", proc_id}, {"started", true}});
            }

            // Foreground execution — routed through the installed
            // terminal env factory (or LocalEnvironment by default).
            auto env = resolve_terminal_env(env_name_from_ctx(ctx));
            hermes::environments::ExecuteOptions opts;
            opts.timeout = std::chrono::seconds(timeout);
            if (!ctx.cwd.empty()) opts.cwd = ctx.cwd;

            auto result = env->execute(command, opts);

            return tool_result(
                {{"stdout", hermes::core::ansi_strip::strip_ansi(result.stdout_text)},
                 {"stderr", hermes::core::ansi_strip::strip_ansi(result.stderr_text)},
                 {"exit_code", result.exit_code},
                 {"timed_out", result.timed_out}});
        };
        reg.register_tool(std::move(entry));
    }

    // -- process -----------------------------------------------------------
    {
        json schema = {
            {"type", "object"},
            {"properties",
             {{"process_id",
               {{"type", "string"}, {"description", "Process ID to query"}}},
              {"action",
               {{"type", "string"},
                {"description", "Action: status, output, or kill"}}}}},
            {"required", json::array({"process_id", "action"})}};

        ToolEntry entry;
        entry.name = "process";
        entry.toolset = "terminal";
        entry.schema = std::move(schema);
        entry.description = "Manage a background process";
        entry.emoji = "\xE2\x9A\x99\xEF\xB8\x8F";  // gear
        entry.max_result_size_chars = 100 * 1024;
        entry.handler = [](const json& args, const ToolContext& /*ctx*/) -> std::string {
            std::string proc_id = args.at("process_id").get<std::string>();
            std::string action = args.at("action").get<std::string>();

            auto& preg = proc_registry();
            auto session = preg.get(proc_id);
            if (!session) {
                return tool_error("unknown process_id: " + proc_id);
            }

            if (action == "status") {
                std::string state_str;
                switch (session->state) {
                    case hermes::state::ProcessState::Running:
                        state_str = "running"; break;
                    case hermes::state::ProcessState::Exited:
                        state_str = "exited"; break;
                    case hermes::state::ProcessState::Killed:
                        state_str = "killed"; break;
                }
                json result = {{"state", state_str}};
                if (session->exit_code.has_value()) {
                    result["exit_code"] = session->exit_code.value();
                }
                return tool_result(result);
            }

            if (action == "output") {
                std::string state_str = "running";
                if (session->state == hermes::state::ProcessState::Exited)
                    state_str = "exited";
                else if (session->state == hermes::state::ProcessState::Killed)
                    state_str = "killed";

                // Return last 10KB of output buffer.
                std::string output = session->output_buffer;
                if (output.size() > 10240) {
                    output = output.substr(output.size() - 10240);
                }
                return tool_result(
                    {{"output", output}, {"state", state_str}});
            }

            if (action == "kill") {
                preg.kill(proc_id);
                return tool_result({{"killed", true}});
            }

            return tool_error("unknown action: " + action +
                              " (expected status, output, or kill)");
        };
        reg.register_tool(std::move(entry));
    }
}

// ── Helpers (terminal::) ──────────────────────────────────────────────

namespace terminal {

std::string safe_command_preview(std::string_view command, std::size_t limit) {
    if (command.empty()) return "<empty>";
    if (command.size() <= limit) return std::string(command);
    return std::string(command.substr(0, limit)) + "...";
}

bool looks_like_env_assignment(std::string_view token) {
    if (token.empty()) return false;
    if (token.front() == '=') return false;
    auto eq = token.find('=');
    if (eq == std::string_view::npos) return false;
    auto name = token.substr(0, eq);
    if (name.empty()) return false;
    if (!(std::isalpha(static_cast<unsigned char>(name[0])) || name[0] == '_')) {
        return false;
    }
    for (std::size_t i = 1; i < name.size(); ++i) {
        unsigned char c = name[i];
        if (!std::isalnum(c) && c != '_') return false;
    }
    return true;
}

std::pair<std::string, std::size_t> read_shell_token(
    std::string_view command, std::size_t start) {
    std::size_t i = start;
    std::size_t n = command.size();

    while (i < n) {
        char ch = command[i];
        if (std::isspace(static_cast<unsigned char>(ch)) || ch == ';' ||
            ch == '|' || ch == '&' || ch == '(' || ch == ')') {
            break;
        }
        if (ch == '\'') {
            ++i;
            while (i < n && command[i] != '\'') ++i;
            if (i < n) ++i;
            continue;
        }
        if (ch == '"') {
            ++i;
            while (i < n) {
                char inner = command[i];
                if (inner == '\\' && i + 1 < n) {
                    i += 2;
                    continue;
                }
                if (inner == '"') {
                    ++i;
                    break;
                }
                ++i;
            }
            continue;
        }
        if (ch == '\\' && i + 1 < n) {
            i += 2;
            continue;
        }
        ++i;
    }
    return {std::string(command.substr(start, i - start)), i};
}

std::pair<std::string, bool> rewrite_real_sudo_invocations(
    std::string_view command) {
    std::string out;
    std::size_t i = 0;
    std::size_t n = command.size();
    bool command_start = true;
    bool found = false;

    while (i < n) {
        char ch = command[i];
        if (std::isspace(static_cast<unsigned char>(ch))) {
            out += ch;
            if (ch == '\n') command_start = true;
            ++i;
            continue;
        }
        if (ch == '#' && command_start) {
            auto nl = command.find('\n', i);
            if (nl == std::string_view::npos) {
                out.append(command.substr(i));
                break;
            }
            out.append(command.substr(i, nl - i));
            i = nl;
            continue;
        }
        if (i + 1 < n &&
            (command.compare(i, 2, "&&") == 0 ||
             command.compare(i, 2, "||") == 0 ||
             command.compare(i, 2, ";;") == 0)) {
            out.append(command.substr(i, 2));
            i += 2;
            command_start = true;
            continue;
        }
        if (ch == ';' || ch == '|' || ch == '&' || ch == '(') {
            out += ch;
            ++i;
            command_start = true;
            continue;
        }
        if (ch == ')') {
            out += ch;
            ++i;
            command_start = false;
            continue;
        }

        auto [token, next_i] = read_shell_token(command, i);
        if (command_start && token == "sudo") {
            out += "sudo -S -p ''";
            found = true;
        } else {
            out += token;
        }

        if (command_start && looks_like_env_assignment(token)) {
            command_start = true;
        } else {
            command_start = false;
        }
        i = next_i;
    }
    return {out, found};
}

std::optional<std::string> interpret_exit_code(std::string_view command,
                                               int exit_code) {
    if (exit_code == 0) return std::nullopt;

    // Extract last segment split on shell chain/pipe operators.
    std::string cmd(command);
    static const std::regex re(R"(\s*(?:\|\||&&|[|;])\s*)");
    std::sregex_token_iterator it(cmd.begin(), cmd.end(), re, -1), end;
    std::string last;
    for (; it != end; ++it) last = *it;
    // trim
    auto trim_begin = last.find_first_not_of(" \t\r\n");
    auto trim_end = last.find_last_not_of(" \t\r\n");
    if (trim_begin == std::string::npos) return std::nullopt;
    last = last.substr(trim_begin, trim_end - trim_begin + 1);
    if (last.empty()) return std::nullopt;

    // First non-env token.
    std::string base;
    std::size_t p = 0;
    while (p < last.size()) {
        while (p < last.size() &&
               std::isspace(static_cast<unsigned char>(last[p]))) {
            ++p;
        }
        auto [tok, q] = read_shell_token(last, p);
        if (tok.empty()) break;
        if (!tok.empty() && tok.front() != '-' && looks_like_env_assignment(tok)) {
            p = q;
            continue;
        }
        // strip leading path.
        auto slash = tok.rfind('/');
        base = (slash == std::string::npos) ? tok : tok.substr(slash + 1);
        break;
    }
    if (base.empty()) return std::nullopt;

    using Map = std::unordered_map<int, std::string>;
    static const std::unordered_map<std::string, Map> semantics = {
        {"grep",  {{1, "No matches found (not an error)"}}},
        {"egrep", {{1, "No matches found (not an error)"}}},
        {"fgrep", {{1, "No matches found (not an error)"}}},
        {"rg",    {{1, "No matches found (not an error)"}}},
        {"ag",    {{1, "No matches found (not an error)"}}},
        {"ack",   {{1, "No matches found (not an error)"}}},
        {"diff",  {{1, "Files differ (expected, not an error)"}}},
        {"colordiff", {{1, "Files differ (expected, not an error)"}}},
        {"find",  {{1, "Some directories were inaccessible"
                       " (partial results may still be valid)"}}},
        {"test",  {{1, "Condition evaluated to false (expected, not an error)"}}},
        {"[",     {{1, "Condition evaluated to false (expected, not an error)"}}},
        {"curl",  {{6,  "Could not resolve host"},
                   {7,  "Failed to connect to host"},
                   {22, "HTTP response code indicated error "
                        "(e.g. 404, 500)"},
                   {28, "Operation timed out"}}},
        {"git",   {{1, "Non-zero exit (often normal — e.g. 'git diff' "
                        "returns 1 when files differ)"}}},
    };
    auto it2 = semantics.find(base);
    if (it2 == semantics.end()) return std::nullopt;
    auto jt = it2->second.find(exit_code);
    if (jt == it2->second.end()) return std::nullopt;
    return jt->second;
}

bool command_requires_pipe_stdin(std::string_view command) {
    // Lowercase normalize + collapse whitespace.
    std::string norm;
    norm.reserve(command.size());
    bool in_space = false;
    for (char c : command) {
        if (std::isspace(static_cast<unsigned char>(c))) {
            if (!in_space && !norm.empty()) norm += ' ';
            in_space = true;
        } else {
            norm += static_cast<char>(
                std::tolower(static_cast<unsigned char>(c)));
            in_space = false;
        }
    }
    auto starts_with = [&](std::string_view p) {
        return norm.size() >= p.size() && norm.compare(0, p.size(), p) == 0;
    };
    return starts_with("gh auth login") &&
           norm.find("--with-token") != std::string::npos;
}

WorkdirResult validate_workdir(std::string_view workdir) {
    WorkdirResult r;
    if (workdir.empty()) {
        r.error = "workdir must not be empty";
        return r;
    }
    fs::path p(workdir);
    std::error_code ec;
    if (!p.is_absolute()) {
        r.error = "workdir must be an absolute path: " + std::string(workdir);
        return r;
    }
    if (!fs::exists(p, ec)) {
        r.error = "workdir does not exist: " + std::string(workdir);
        return r;
    }
    if (!fs::is_directory(p, ec)) {
        r.error = "workdir is not a directory: " + std::string(workdir);
        return r;
    }
    auto canon = fs::canonical(p, ec);
    r.path = ec ? p.string() : canon.string();
    return r;
}

int clamp_timeout(int requested) {
    if (requested < 1) return 1;
    if (requested > 3600) return 3600;
    return requested;
}

}  // namespace terminal

}  // namespace hermes::tools
