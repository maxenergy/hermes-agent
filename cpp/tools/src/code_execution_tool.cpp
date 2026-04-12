#include "hermes/tools/code_execution_tool.hpp"

#include "hermes/environments/local.hpp"
#include "hermes/tools/registry.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <random>
#include <sstream>
#include <string>

namespace hermes::tools {

using json = nlohmann::json;
namespace fs = std::filesystem;

namespace {

std::string generate_uuid() {
    static std::mt19937 gen(std::random_device{}());
    std::uniform_int_distribution<> dis(0, 15);
    const char hex[] = "0123456789abcdef";
    std::string uuid;
    uuid.reserve(32);
    for (int i = 0; i < 32; ++i) {
        uuid += hex[dis(gen)];
    }
    return uuid;
}

// Write the hermes_tools.py stub so sandboxed Python code can import it.
void write_python_stub(const fs::path& dir) {
    auto stub_path = dir / "hermes_tools.py";
    if (fs::exists(stub_path)) return;

    std::ofstream ofs(stub_path);
    ofs << R"PY("""hermes_tools — sandbox stub. All functions return a not-available message."""

def web_search(*a, **kw):
    return "tool not available in sandbox"

def web_extract(*a, **kw):
    return "tool not available in sandbox"

def read_file(*a, **kw):
    return "tool not available in sandbox"

def write_file(*a, **kw):
    return "tool not available in sandbox"

def search_files(*a, **kw):
    return "tool not available in sandbox"

def patch(*a, **kw):
    return "tool not available in sandbox"

def terminal(*a, **kw):
    return "tool not available in sandbox"
)PY";
}

constexpr std::size_t MAX_OUTPUT_BYTES = 50 * 1024;  // 50 KB

std::string truncate_output(const std::string& s) {
    if (s.size() <= MAX_OUTPUT_BYTES) return s;
    return s.substr(0, MAX_OUTPUT_BYTES) + "\n... (truncated at 50KB)";
}

}  // namespace

void register_code_execution_tools(ToolRegistry& registry) {
    ToolEntry e;
    e.name = "execute_code";
    e.toolset = "code";
    e.description = "Execute Python or Bash code and return the output";
    e.emoji = "\xf0\x9f\x92\xbb";  // computer

    e.schema = json::parse(R"JSON({
        "type": "object",
        "properties": {
            "language": {
                "type": "string",
                "enum": ["python", "bash"],
                "description": "Language to execute"
            },
            "code": {
                "type": "string",
                "description": "Code to execute"
            },
            "timeout": {
                "type": "integer",
                "description": "Timeout in seconds (default 300)",
                "default": 300
            }
        },
        "required": ["language", "code"]
    })JSON");

    e.handler = [](const json& args, const ToolContext& ctx) -> std::string {
        if (!args.contains("language") || !args["language"].is_string()) {
            return tool_error("missing required parameter: language");
        }
        if (!args.contains("code") || !args["code"].is_string()) {
            return tool_error("missing required parameter: code");
        }

        auto language = args["language"].get<std::string>();
        auto code = args["code"].get<std::string>();
        int timeout = args.value("timeout", 300);

        if (language != "python" && language != "bash") {
            return tool_error("unsupported language: " + language +
                              " (must be 'python' or 'bash')");
        }

        // Generate temp file path.
        auto uuid = generate_uuid();
        std::string ext = (language == "python") ? ".py" : ".sh";
        auto script_path = fs::path("/tmp") / ("hermes_exec_" + uuid + ext);

        // Write the code to the temp file.
        {
            std::ofstream ofs(script_path);
            if (!ofs.is_open()) {
                return tool_error("failed to create temp script file");
            }
            ofs << code;
        }

        // For Python, write the hermes_tools stub in the same directory.
        if (language == "python") {
            write_python_stub(script_path.parent_path());
        }

        // Build the command.
        std::string cmd;
        if (language == "python") {
            cmd = "cd /tmp && python3 " + script_path.string();
        } else {
            cmd = "bash " + script_path.string();
        }

        // Execute.
        hermes::environments::LocalEnvironment env;
        hermes::environments::ExecuteOptions opts;
        opts.timeout = std::chrono::seconds(timeout);
        if (!ctx.cwd.empty()) {
            opts.cwd = ctx.cwd;
        }

        hermes::environments::CompletedProcess result;
        try {
            result = env.execute(cmd, opts);
        } catch (const std::exception& ex) {
            // Clean up and report.
            std::error_code ec;
            fs::remove(script_path, ec);
            return tool_error(std::string("execution failed: ") + ex.what());
        }

        // Clean up temp file.
        {
            std::error_code ec;
            fs::remove(script_path, ec);
        }

        return tool_result({
            {"stdout", truncate_output(result.stdout_text)},
            {"stderr", truncate_output(result.stderr_text)},
            {"exit_code", result.exit_code},
            {"timed_out", result.timed_out}
        });
    };

    registry.register_tool(std::move(e));
}

}  // namespace hermes::tools
