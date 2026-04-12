#include "hermes/tools/terminal_tool.hpp"
#include "hermes/tools/registry.hpp"
#include "hermes/environments/local.hpp"
#include "hermes/state/process_registry.hpp"
#include "hermes/core/ansi_strip.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace hermes::tools {

using json = nlohmann::json;
namespace fs = std::filesystem;

namespace {

// Module-level process registry for background processes.
std::mutex g_proc_mu;
std::unique_ptr<hermes::state::ProcessRegistry> g_proc_reg;

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
                std::thread([proc_id, command, timeout, cwd]() {
                    hermes::environments::LocalEnvironment env;
                    hermes::environments::ExecuteOptions opts;
                    opts.timeout = std::chrono::seconds(timeout);
                    if (!cwd.empty()) opts.cwd = cwd;

                    auto result = env.execute(command, opts);

                    auto& preg = proc_registry();
                    preg.feed_output(proc_id,
                                     result.stdout_text + result.stderr_text);
                    preg.mark_exited(proc_id, result.exit_code);
                }).detach();

                return tool_result({{"process_id", proc_id}, {"started", true}});
            }

            // Foreground execution.
            hermes::environments::LocalEnvironment env;
            hermes::environments::ExecuteOptions opts;
            opts.timeout = std::chrono::seconds(timeout);
            if (!ctx.cwd.empty()) opts.cwd = ctx.cwd;

            auto result = env.execute(command, opts);

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

}  // namespace hermes::tools
