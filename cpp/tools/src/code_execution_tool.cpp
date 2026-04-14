// Code execution tool — port of tools/code_execution_tool.py.
//
// Responsibilities:
//   - write a temp script (Python or Bash)
//   - emit the hermes_tools shim alongside it for Python executions
//   - run the script via the LocalEnvironment with an enforced timeout
//   - truncate oversize output and return structured JSON
//   - expose a handful of helpers (shell_quote, truncate_output,
//     generate_hermes_tools_module, build_execute_code_schema) used by
//     the tool and by tests.
#include "hermes/tools/code_execution_tool.hpp"

#include "hermes/environments/local.hpp"
#include "hermes/tools/registry.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <random>
#include <sstream>
#include <string>
#include <string_view>

namespace hermes::tools::code_execution {

namespace fs = std::filesystem;
using json = nlohmann::json;

// ── Sandbox allow-list ───────────────────────────────────────────────

const std::unordered_set<std::string>& sandbox_allowed_tools() {
    static const std::unordered_set<std::string> s{
        "web_search", "web_extract",
        "read_file", "write_file", "patch", "search_files",
        "terminal",  "send_message",
        "browser_navigate", "browser_click", "browser_extract",
        "memory_get", "memory_set", "memory_search",
        "todo_add", "todo_list", "todo_done",
        "image_generation", "vision",
        "tts", "transcription",
    };
    return s;
}

// ── Output truncation / clamping ─────────────────────────────────────

std::string truncate_output(std::string_view s, std::size_t max_bytes) {
    if (s.size() <= max_bytes) return std::string(s);
    std::string out(s.substr(0, max_bytes));
    out += "\n... (truncated at " + std::to_string(max_bytes) + " bytes)";
    return out;
}

int clamp_timeout(int requested) {
    if (requested < kMinTimeoutSeconds) return kMinTimeoutSeconds;
    if (requested > kMaxTimeoutSeconds) return kMaxTimeoutSeconds;
    return requested;
}

// ── Shell quoting (shlex.quote equivalent) ───────────────────────────

std::string shell_quote(std::string_view s) {
    if (s.empty()) return "''";
    bool safe = true;
    for (char c : s) {
        // POSIX-safe set: letters, digits, @, %, +, =, :, ,, ., /, -, _.
        auto uc = static_cast<unsigned char>(c);
        if (std::isalnum(uc)) continue;
        if (c == '@' || c == '%' || c == '+' || c == '=' || c == ':' ||
            c == ',' || c == '.' || c == '/' || c == '-' || c == '_') {
            continue;
        }
        safe = false;
        break;
    }
    if (safe) return std::string(s);

    std::string out = "'";
    for (char c : s) {
        if (c == '\'') {
            out += "'\\''";  // close, escaped quote, reopen
        } else {
            out += c;
        }
    }
    out += "'";
    return out;
}

// ── UUID generator ───────────────────────────────────────────────────

std::string generate_uuid() {
    static thread_local std::mt19937_64 gen(
        std::random_device{}() ^
        static_cast<std::uint64_t>(
            std::chrono::steady_clock::now().time_since_epoch().count()));
    std::uniform_int_distribution<int> dis(0, 15);
    static constexpr char hex[] = "0123456789abcdef";
    std::string uuid;
    uuid.reserve(32);
    for (int i = 0; i < 32; ++i) uuid += hex[dis(gen)];
    return uuid;
}

// ── hermes_tools.py module generator ─────────────────────────────────

namespace {

// Tool → stub signature description.  Each tuple is
// (stub_name, signature, docstring, args_expr).
struct ToolStub {
    std::string signature;
    std::string docstring;
    std::string args_expr;
};

const std::unordered_map<std::string, ToolStub>& tool_stubs() {
    static const std::unordered_map<std::string, ToolStub> m = {
        {"web_search",
         {"query: str, num_results: int = 5, provider: str = None",
          "\"\"\"Search the web via the configured provider.\"\"\"",
          "{'query': query, 'num_results': num_results, "
          "'provider': provider}"}},
        {"web_extract",
         {"url: str, max_length: int = 10000",
          "\"\"\"Extract article content from a URL.\"\"\"",
          "{'url': url, 'max_length': max_length}"}},
        {"read_file",
         {"path: str",
          "\"\"\"Read a text file from disk.\"\"\"",
          "{'path': path}"}},
        {"write_file",
         {"path: str, content: str",
          "\"\"\"Write (overwrite) a text file to disk.\"\"\"",
          "{'path': path, 'content': content}"}},
        {"patch",
         {"path: str, diff: str",
          "\"\"\"Apply a unified diff to a file.\"\"\"",
          "{'path': path, 'diff': diff}"}},
        {"search_files",
         {"pattern: str, path: str = '.'",
          "\"\"\"Search files by content pattern.\"\"\"",
          "{'pattern': pattern, 'path': path}"}},
        {"terminal",
         {"command: str, timeout: int = 120, background: bool = False",
          "\"\"\"Run a shell command.\"\"\"",
          "{'command': command, 'timeout': timeout, 'background': background}"}},
        {"send_message",
         {"target: str, message: str",
          "\"\"\"Send a message to a connected platform.\"\"\"",
          "{'action': 'send', 'target': target, 'message': message}"}},
    };
    return m;
}

constexpr const char* kCommonHelpers = R"PY(

# ---------------------------------------------------------------------------
# Convenience helpers (avoid common scripting pitfalls)
# ---------------------------------------------------------------------------

def json_parse(text):
    """Parse JSON tolerant of control characters (strict=False).
    Use this instead of json.loads() when parsing output from terminal()
    or web_extract() that may contain raw tabs/newlines in strings."""
    return json.loads(text, strict=False)


def shell_quote(s):
    """Shell-escape a string for safe interpolation into commands."""
    return shlex.quote(s)


def retry(fn, max_attempts=3, delay=2):
    """Retry a function up to max_attempts times with exponential backoff."""
    last_err = None
    for attempt in range(max_attempts):
        try:
            return fn()
        except Exception as e:
            last_err = e
            if attempt < max_attempts - 1:
                time.sleep(delay * (2 ** attempt))
    raise last_err

)PY";

constexpr const char* kUdsTransportHeader = R"PY("""Auto-generated Hermes tools RPC stubs (UDS transport)."""
import json
import os
import shlex
import socket
import time


def _connect():
    sock_path = os.environ.get("HERMES_RPC_SOCK", "/tmp/hermes.sock")
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.connect(sock_path)
    return s


def _call(tool_name, args):
    payload = json.dumps({"tool": tool_name, "args": args}).encode("utf-8")
    s = _connect()
    try:
        s.sendall(payload + b"\n")
        buf = b""
        while True:
            chunk = s.recv(65536)
            if not chunk:
                break
            buf += chunk
            if b"\n" in chunk:
                break
        return json.loads(buf.decode("utf-8").strip(), strict=False)
    finally:
        s.close()

)PY";

constexpr const char* kFileTransportHeader = R"PY("""Auto-generated Hermes tools RPC stubs (file transport)."""
import json
import os
import shlex
import time
import uuid


def _call(tool_name, args):
    req_dir = os.environ.get("HERMES_RPC_REQ_DIR", "/tmp/hermes_req")
    resp_dir = os.environ.get("HERMES_RPC_RESP_DIR", "/tmp/hermes_resp")
    os.makedirs(req_dir, exist_ok=True)
    os.makedirs(resp_dir, exist_ok=True)
    rid = uuid.uuid4().hex
    req_path = os.path.join(req_dir, rid + ".req.json")
    resp_path = os.path.join(resp_dir, rid + ".resp.json")
    with open(req_path, "w", encoding="utf-8") as f:
        json.dump({"tool": tool_name, "args": args, "id": rid}, f)
    # Poll for a response.
    deadline = time.time() + 600
    while time.time() < deadline:
        if os.path.exists(resp_path):
            with open(resp_path, "r", encoding="utf-8") as f:
                return json.loads(f.read(), strict=False)
        time.sleep(0.1)
    raise TimeoutError("RPC call timed out: " + tool_name)

)PY";

}  // namespace

std::string generate_hermes_tools_module(
    const std::vector<std::string>& enabled_tools,
    std::string_view transport) {
    std::vector<std::string> keep;
    const auto& allowed = sandbox_allowed_tools();
    const auto& stubs = tool_stubs();
    keep.reserve(enabled_tools.size());
    for (const auto& t : enabled_tools) {
        if (allowed.count(t) && stubs.count(t)) keep.push_back(t);
    }
    std::sort(keep.begin(), keep.end());

    std::ostringstream oss;
    if (transport == "file") {
        oss << kFileTransportHeader;
    } else {
        oss << kUdsTransportHeader;
    }
    oss << kCommonHelpers;
    for (const auto& name : keep) {
        const auto& stub = stubs.at(name);
        oss << "def " << name << "(" << stub.signature << "):\n";
        oss << "    " << stub.docstring << "\n";
        oss << "    return _call(" << std::quoted(name) << ", "
            << stub.args_expr << ")\n\n";
    }
    return oss.str();
}

// ── Schema builder ───────────────────────────────────────────────────

json build_execute_code_schema(
    const std::vector<std::string>& enabled_sandbox_tools) {
    json schema = json::parse(R"JSON({
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
                "description": "Timeout in seconds (default 300, max 3600)",
                "default": 300,
                "minimum": 1,
                "maximum": 3600
            }
        },
        "required": ["language", "code"]
    })JSON");

    if (!enabled_sandbox_tools.empty()) {
        std::vector<std::string> allowed;
        const auto& allow = sandbox_allowed_tools();
        for (const auto& t : enabled_sandbox_tools) {
            if (allow.count(t)) allowed.push_back(t);
        }
        std::sort(allowed.begin(), allowed.end());
        if (!allowed.empty()) {
            std::string list;
            for (std::size_t i = 0; i < allowed.size(); ++i) {
                if (i) list += ", ";
                list += allowed[i];
            }
            schema["description"] =
                "Execute Python or Bash. Python code may call "
                "hermes_tools.* RPC stubs for: " +
                list;
        }
    }
    return schema;
}

// ── Requirement check ────────────────────────────────────────────────

bool check_sandbox_requirements() {
    // Best-effort: we probe python3 via the shell.
    const char* path = std::getenv("PATH");
    if (!path) return false;
    std::string s(path);
    std::string token;
    for (std::size_t i = 0; i <= s.size(); ++i) {
        if (i == s.size() || s[i] == ':') {
            if (!token.empty()) {
                fs::path p = token;
                std::error_code ec;
                if (fs::exists(p / "python3", ec)) return true;
                if (fs::exists(p / "python", ec)) return true;
            }
            token.clear();
        } else {
            token += s[i];
        }
    }
    return false;
}

// ── Registration ─────────────────────────────────────────────────────

namespace {

void write_python_shim(const fs::path& dir,
                       const std::vector<std::string>& enabled_tools) {
    auto shim_path = dir / "hermes_tools.py";
    std::error_code ec;
    if (fs::exists(shim_path, ec)) return;
    std::ofstream ofs(shim_path);
    if (!ofs.is_open()) return;
    if (enabled_tools.empty()) {
        // Default — emit a tiny "restricted" shim so import still works.
        ofs << "\"\"\"hermes_tools — sandbox shim (restricted).\"\"\"\n\n";
        ofs << "def _restricted(*a, **kw):\n"
               "    raise NotImplementedError("
               "'tool restricted in sandbox')\n\n";
        for (const auto& name : sandbox_allowed_tools()) {
            ofs << name << " = _restricted\n";
        }
    } else {
        ofs << generate_hermes_tools_module(enabled_tools, "uds");
    }
}

}  // namespace

void register_code_execution_tools(hermes::tools::ToolRegistry& registry) {
    ToolEntry e;
    e.name = "execute_code";
    e.toolset = "code";
    e.description =
        "Execute Python or Bash code and return the output";
    e.emoji = "\xf0\x9f\x92\xbb";  // computer
    e.schema = build_execute_code_schema();
    e.check_fn = &check_sandbox_requirements;

    e.handler = [](const json& args, const ToolContext& ctx) -> std::string {
        if (!args.contains("language") || !args["language"].is_string()) {
            return tool_error("missing required parameter: language");
        }
        if (!args.contains("code") || !args["code"].is_string()) {
            return tool_error("missing required parameter: code");
        }
        auto language = args["language"].get<std::string>();
        auto code = args["code"].get<std::string>();
        int timeout = clamp_timeout(args.value("timeout", kDefaultTimeoutSeconds));
        if (language != "python" && language != "bash") {
            return tool_error(
                "unsupported language: " + language +
                " (must be 'python' or 'bash')");
        }

        auto uuid = generate_uuid();
        std::string ext = (language == "python") ? ".py" : ".sh";
        auto script_path = fs::path("/tmp") / ("hermes_exec_" + uuid + ext);

        {
            std::ofstream ofs(script_path);
            if (!ofs.is_open()) {
                return tool_error("failed to create temp script file");
            }
            ofs << code;
        }

        if (language == "python") {
            std::vector<std::string> empty;
            write_python_shim(script_path.parent_path(), empty);
        }

        std::string cmd;
        if (language == "python") {
            cmd = "cd /tmp && python3 " + shell_quote(script_path.string());
        } else {
            cmd = "bash " + shell_quote(script_path.string());
        }

        hermes::environments::LocalEnvironment env;
        hermes::environments::ExecuteOptions opts;
        opts.timeout = std::chrono::seconds(timeout);
        if (!ctx.cwd.empty()) opts.cwd = ctx.cwd;

        hermes::environments::CompletedProcess result;
        try {
            result = env.execute(cmd, opts);
        } catch (const std::exception& ex) {
            std::error_code ec;
            fs::remove(script_path, ec);
            return tool_error(std::string("execution failed: ") + ex.what());
        }

        std::error_code ec;
        fs::remove(script_path, ec);

        return tool_result({
            {"stdout", truncate_output(result.stdout_text)},
            {"stderr", truncate_output(result.stderr_text)},
            {"exit_code", result.exit_code},
            {"timed_out", result.timed_out},
            {"language", language},
            {"timeout", timeout}});
    };
    registry.register_tool(std::move(e));
}

}  // namespace hermes::tools::code_execution
