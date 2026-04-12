#include "hermes/tools/mcp_transport.hpp"

#include "hermes/environments/env_filter.hpp"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdexcept>
#else
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#include <array>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <unordered_map>

namespace hermes::tools {

using json = nlohmann::json;

// ---------------------------------------------------------------------------
// JsonRpcMessage
// ---------------------------------------------------------------------------

json JsonRpcMessage::to_json() const {
    json j;
    j["jsonrpc"] = jsonrpc;
    if (id.has_value()) {
        j["id"] = std::stoi(*id);
    }
    if (!method.empty()) {
        j["method"] = method;
    }
    if (!params.is_null()) {
        j["params"] = params;
    }
    if (!result.is_null()) {
        j["result"] = result;
    }
    if (!error.is_null()) {
        j["error"] = error;
    }
    return j;
}

JsonRpcMessage JsonRpcMessage::from_json(const json& j) {
    JsonRpcMessage msg;
    msg.jsonrpc = j.value("jsonrpc", "2.0");
    if (j.contains("id")) {
        if (j["id"].is_number()) {
            msg.id = std::to_string(j["id"].get<int>());
        } else if (j["id"].is_string()) {
            msg.id = j["id"].get<std::string>();
        }
    }
    msg.method = j.value("method", "");
    if (j.contains("params")) msg.params = j["params"];
    if (j.contains("result")) msg.result = j["result"];
    if (j.contains("error")) msg.error = j["error"];
    return msg;
}

// ---------------------------------------------------------------------------
// McpStdioTransport
// ---------------------------------------------------------------------------

#ifdef _WIN32

namespace {

// Track HANDLEs out-of-band keyed by the instance pointer — the class stores
// int fields for POSIX compatibility, so we stash the real Win32 handles in a
// file-scope map. This keeps the header cross-platform (no <windows.h> leak).
struct WinHandles {
    HANDLE proc = nullptr;
    HANDLE stdin_wr = nullptr;
    HANDLE stdout_rd = nullptr;
};
static std::mutex g_handles_mu;
static std::unordered_map<const void*, WinHandles>& g_handles() {
    static std::unordered_map<const void*, WinHandles> m;
    return m;
}

std::string quote_arg(const std::string& a) {
    if (a.find_first_of(" \t\"") == std::string::npos) return a;
    std::string out = "\"";
    for (char c : a) {
        if (c == '"') out += '\\';
        out += c;
    }
    out += '"';
    return out;
}

}  // namespace

McpStdioTransport::McpStdioTransport(
    const std::string& command,
    const std::vector<std::string>& args,
    const std::unordered_map<std::string, std::string>& env) {

    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    HANDLE child_stdin_rd = nullptr, child_stdin_wr = nullptr;
    HANDLE child_stdout_rd = nullptr, child_stdout_wr = nullptr;

    if (!CreatePipe(&child_stdin_rd, &child_stdin_wr, &sa, 0) ||
        !CreatePipe(&child_stdout_rd, &child_stdout_wr, &sa, 0)) {
        throw std::runtime_error("McpStdioTransport: CreatePipe failed");
    }
    SetHandleInformation(child_stdin_wr, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(child_stdout_rd, HANDLE_FLAG_INHERIT, 0);

    std::string cmdline = quote_arg(command);
    for (const auto& a : args) { cmdline += ' '; cmdline += quote_arg(a); }
    std::vector<char> cmd_mut(cmdline.begin(), cmdline.end());
    cmd_mut.push_back('\0');

    // Build env block: key=value\0 key=value\0 \0
    auto child_env = build_child_env(env);
    std::string env_block;
    for (const auto& [k, v] : child_env) {
        env_block += k; env_block += '='; env_block += v; env_block.push_back('\0');
    }
    env_block.push_back('\0');

    STARTUPINFOA si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = child_stdin_rd;
    si.hStdOutput = child_stdout_wr;
    si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    PROCESS_INFORMATION pi{};

    BOOL ok = CreateProcessA(nullptr, cmd_mut.data(), nullptr, nullptr, TRUE,
                             0, env_block.data(), nullptr, &si, &pi);
    CloseHandle(child_stdin_rd);
    CloseHandle(child_stdout_wr);
    if (!ok) {
        CloseHandle(child_stdin_wr);
        CloseHandle(child_stdout_rd);
        throw std::runtime_error("McpStdioTransport: CreateProcess failed");
    }
    CloseHandle(pi.hThread);

    {
        std::lock_guard<std::mutex> lk(g_handles_mu);
        g_handles()[this] = {pi.hProcess, child_stdin_wr, child_stdout_rd};
    }
    child_pid_ = static_cast<int>(pi.dwProcessId);
    stdin_fd_ = 1;
    stdout_fd_ = 1;
}

McpStdioTransport::~McpStdioTransport() { shutdown(); }

bool McpStdioTransport::is_connected() const {
    std::lock_guard<std::mutex> lk(g_handles_mu);
    auto it = g_handles().find(this);
    if (it == g_handles().end() || !it->second.proc) return false;
    DWORD wait = WaitForSingleObject(it->second.proc, 0);
    return wait == WAIT_TIMEOUT;
}

void McpStdioTransport::write_message(const json& msg) {
    std::string line = msg.dump() + "\n";
    HANDLE h;
    {
        std::lock_guard<std::mutex> lk(g_handles_mu);
        h = g_handles()[this].stdin_wr;
    }
    if (!h) throw std::runtime_error("McpStdioTransport: not connected");
    DWORD written = 0;
    if (!WriteFile(h, line.data(), (DWORD)line.size(), &written, nullptr)) {
        throw std::runtime_error("McpStdioTransport: WriteFile failed");
    }
}

std::optional<json> McpStdioTransport::read_message(std::chrono::seconds timeout) {
    HANDLE h;
    {
        std::lock_guard<std::mutex> lk(g_handles_mu);
        h = g_handles()[this].stdout_rd;
    }
    if (!h) return std::nullopt;

    auto deadline = std::chrono::steady_clock::now() + timeout;
    std::string buffer;
    while (std::chrono::steady_clock::now() < deadline) {
        DWORD avail = 0;
        if (!PeekNamedPipe(h, nullptr, 0, nullptr, &avail, nullptr)) {
            return std::nullopt;
        }
        if (avail > 0) {
            char buf[4096];
            DWORD got = 0;
            DWORD to_read = avail < sizeof(buf) ? avail : (DWORD)sizeof(buf);
            if (!ReadFile(h, buf, to_read, &got, nullptr) || got == 0) {
                return std::nullopt;
            }
            buffer.append(buf, got);
            auto nl = buffer.find('\n');
            if (nl != std::string::npos) {
                std::string line = buffer.substr(0, nl);
                try { return json::parse(line); }
                catch (const json::parse_error&) {
                    buffer = buffer.substr(nl + 1);
                    continue;
                }
            }
        } else {
            Sleep(20);
        }
    }
    return std::nullopt;
}

json McpStdioTransport::send_request(const std::string& method,
                                     const json& params,
                                     std::chrono::seconds timeout) {
    std::lock_guard<std::mutex> lock(mu_);
    int id = next_id_++;
    json msg;
    msg["jsonrpc"] = "2.0";
    msg["id"] = id;
    msg["method"] = method;
    if (!params.is_null()) msg["params"] = params;
    write_message(msg);

    auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        auto remaining = std::chrono::duration_cast<std::chrono::seconds>(
            deadline - std::chrono::steady_clock::now());
        if (remaining.count() <= 0) break;
        auto response = read_message(remaining);
        if (!response) {
            throw std::runtime_error(
                "McpStdioTransport: timeout waiting for response to " + method);
        }
        if (response->contains("id")) {
            int resp_id = -1;
            if ((*response)["id"].is_number()) resp_id = (*response)["id"].get<int>();
            if (resp_id == id) {
                if (response->contains("error") && !(*response)["error"].is_null()) {
                    throw std::runtime_error(
                        "McpStdioTransport: JSON-RPC error: " +
                        (*response)["error"].dump());
                }
                if (response->contains("result")) return (*response)["result"];
                return *response;
            }
        }
    }
    throw std::runtime_error(
        "McpStdioTransport: timeout waiting for response to " + method);
}

void McpStdioTransport::send_notification(const std::string& method,
                                          const json& params) {
    std::lock_guard<std::mutex> lock(mu_);
    json msg;
    msg["jsonrpc"] = "2.0";
    msg["method"] = method;
    if (!params.is_null()) msg["params"] = params;
    write_message(msg);
}

json McpStdioTransport::initialize(const std::string& client_name,
                                   const std::string& client_version) {
    json params;
    params["protocolVersion"] = "2024-11-05";
    params["capabilities"] = json::object();
    params["clientInfo"] = {{"name", client_name}, {"version", client_version}};
    auto result = send_request("initialize", params);
    send_notification("notifications/initialized");
    return result;
}

std::vector<json> McpStdioTransport::list_tools() {
    auto result = send_request("tools/list");
    std::vector<json> tools;
    if (result.contains("tools") && result["tools"].is_array()) {
        for (const auto& t : result["tools"]) tools.push_back(t);
    }
    return tools;
}

json McpStdioTransport::call_tool(const std::string& tool_name, const json& args) {
    json params;
    params["name"] = tool_name;
    params["arguments"] = args;
    return send_request("tools/call", params);
}

void McpStdioTransport::shutdown() {
    std::lock_guard<std::mutex> lk(g_handles_mu);
    auto it = g_handles().find(this);
    if (it == g_handles().end()) return;
    auto& wh = it->second;
    if (wh.stdin_wr) { CloseHandle(wh.stdin_wr); wh.stdin_wr = nullptr; }
    if (wh.stdout_rd) { CloseHandle(wh.stdout_rd); wh.stdout_rd = nullptr; }
    if (wh.proc) {
        if (WaitForSingleObject(wh.proc, 200) != WAIT_OBJECT_0) {
            TerminateProcess(wh.proc, 1);
            WaitForSingleObject(wh.proc, INFINITE);
        }
        CloseHandle(wh.proc);
        wh.proc = nullptr;
    }
    g_handles().erase(it);
    child_pid_ = -1;
    stdin_fd_ = -1;
    stdout_fd_ = -1;
}

std::unordered_map<std::string, std::string> McpStdioTransport::build_child_env(
    const std::unordered_map<std::string, std::string>& server_env) {
    static const std::array<const char*, 5> kSafeVars = {{
        "PATH", "HOME", "LANG", "SHELL", "TMPDIR"
    }};
    std::unordered_map<std::string, std::string> env;
    for (const auto* var : kSafeVars) {
        const char* val = std::getenv(var);
        if (val != nullptr) env[var] = val;
    }
    for (const auto& [k, v] : server_env) env[k] = v;
    return hermes::environments::filter_env(env);
}

#else  // POSIX

McpStdioTransport::McpStdioTransport(
    const std::string& command,
    const std::vector<std::string>& args,
    const std::unordered_map<std::string, std::string>& env) {

    // Create two pipes: parent->child (child stdin), child->parent (child stdout).
    int to_child[2];    // to_child[0] = child reads, to_child[1] = parent writes
    int from_child[2];  // from_child[0] = parent reads, from_child[1] = child writes

    if (::pipe(to_child) != 0 || ::pipe(from_child) != 0) {
        throw std::runtime_error("McpStdioTransport: pipe() failed");
    }

    pid_t pid = ::fork();
    if (pid < 0) {
        ::close(to_child[0]); ::close(to_child[1]);
        ::close(from_child[0]); ::close(from_child[1]);
        throw std::runtime_error("McpStdioTransport: fork() failed");
    }

    if (pid == 0) {
        // -- Child process --
        ::close(to_child[1]);    // close parent's write end
        ::close(from_child[0]);  // close parent's read end

        ::dup2(to_child[0], STDIN_FILENO);
        ::dup2(from_child[1], STDOUT_FILENO);
        ::close(to_child[0]);
        ::close(from_child[1]);

        // Redirect stderr to /dev/null.
        int devnull = ::open("/dev/null", O_WRONLY);
        if (devnull >= 0) {
            ::dup2(devnull, STDERR_FILENO);
            ::close(devnull);
        }

        // Build the filtered environment.
        auto child_env = build_child_env(env);
        std::vector<std::string> env_strings;
        env_strings.reserve(child_env.size());
        for (const auto& [k, v] : child_env) {
            env_strings.push_back(k + "=" + v);
        }
        std::vector<char*> envp;
        envp.reserve(env_strings.size() + 1);
        for (auto& s : env_strings) {
            envp.push_back(s.data());
        }
        envp.push_back(nullptr);

        // Build argv: command + args.
        std::vector<char*> argv;
        // We need a mutable copy of command for execvp.
        std::string cmd_copy = command;
        argv.push_back(cmd_copy.data());
        std::vector<std::string> args_copy(args.begin(), args.end());
        for (auto& a : args_copy) {
            argv.push_back(a.data());
        }
        argv.push_back(nullptr);

        ::execvpe(command.c_str(), argv.data(), envp.data());
        // If execvpe fails, exit.
        _exit(127);
    }

    // -- Parent process --
    ::close(to_child[0]);    // close child's read end
    ::close(from_child[1]);  // close child's write end

    child_pid_ = pid;
    stdin_fd_ = to_child[1];
    stdout_fd_ = from_child[0];
}

McpStdioTransport::~McpStdioTransport() {
    shutdown();
}

bool McpStdioTransport::is_connected() const {
    if (child_pid_ <= 0) return false;
    // Check if child is still running.
    int status = 0;
    pid_t r = ::waitpid(child_pid_, &status, WNOHANG);
    return r == 0;  // 0 means child still running
}

void McpStdioTransport::write_message(const json& msg) {
    std::string line = msg.dump() + "\n";
    const char* data = line.c_str();
    std::size_t remaining = line.size();
    while (remaining > 0) {
        auto n = ::write(stdin_fd_, data, remaining);
        if (n <= 0) {
            throw std::runtime_error("McpStdioTransport: write failed");
        }
        data += n;
        remaining -= static_cast<std::size_t>(n);
    }
}

std::optional<json> McpStdioTransport::read_message(std::chrono::seconds timeout) {
    struct pollfd pfd;
    pfd.fd = stdout_fd_;
    pfd.events = POLLIN;

    auto deadline = std::chrono::steady_clock::now() + timeout;
    std::string buffer;

    while (std::chrono::steady_clock::now() < deadline) {
        auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now());
        if (remaining.count() <= 0) break;

        int ret = ::poll(&pfd, 1, static_cast<int>(remaining.count()));
        if (ret < 0) {
            if (errno == EINTR) continue;
            return std::nullopt;
        }
        if (ret == 0) break;  // timeout

        if (pfd.revents & (POLLIN | POLLHUP)) {
            char buf[4096];
            auto n = ::read(stdout_fd_, buf, sizeof(buf));
            if (n <= 0) return std::nullopt;  // EOF or error

            buffer.append(buf, static_cast<std::size_t>(n));

            // Check for complete line(s).
            auto newline_pos = buffer.find('\n');
            if (newline_pos != std::string::npos) {
                std::string line = buffer.substr(0, newline_pos);
                // We discard the rest — MCP is strictly request/response
                // so we don't expect multiple messages at once in normal use.
                // But keep any leftover for robustness.
                try {
                    return json::parse(line);
                } catch (const json::parse_error&) {
                    // Skip non-JSON lines (e.g. server startup messages).
                    buffer = buffer.substr(newline_pos + 1);
                    continue;
                }
            }
        }
    }

    return std::nullopt;  // timeout
}

json McpStdioTransport::send_request(const std::string& method,
                                     const json& params,
                                     std::chrono::seconds timeout) {
    std::lock_guard<std::mutex> lock(mu_);

    int id = next_id_++;
    json msg;
    msg["jsonrpc"] = "2.0";
    msg["id"] = id;
    msg["method"] = method;
    if (!params.is_null()) {
        msg["params"] = params;
    }

    write_message(msg);

    // Wait for response with matching id.
    auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        auto remaining = std::chrono::duration_cast<std::chrono::seconds>(
            deadline - std::chrono::steady_clock::now());
        if (remaining.count() <= 0) break;

        auto response = read_message(remaining);
        if (!response.has_value()) {
            throw std::runtime_error(
                "McpStdioTransport: timeout waiting for response to " + method);
        }

        // Check if this is our response.
        if (response->contains("id")) {
            int resp_id = -1;
            if ((*response)["id"].is_number()) {
                resp_id = (*response)["id"].get<int>();
            } else if ((*response)["id"].is_string()) {
                try { resp_id = std::stoi((*response)["id"].get<std::string>()); }
                catch (...) {}
            }
            if (resp_id == id) {
                if (response->contains("error") && !(*response)["error"].is_null()) {
                    throw std::runtime_error(
                        "McpStdioTransport: JSON-RPC error: " +
                        (*response)["error"].dump());
                }
                if (response->contains("result")) {
                    return (*response)["result"];
                }
                return *response;
            }
        }
        // Not our response — could be a notification from server; skip it.
    }

    throw std::runtime_error(
        "McpStdioTransport: timeout waiting for response to " + method);
}

void McpStdioTransport::send_notification(const std::string& method,
                                          const json& params) {
    std::lock_guard<std::mutex> lock(mu_);

    json msg;
    msg["jsonrpc"] = "2.0";
    msg["method"] = method;
    if (!params.is_null()) {
        msg["params"] = params;
    }

    write_message(msg);
}

json McpStdioTransport::initialize(const std::string& client_name,
                                   const std::string& client_version) {
    json params;
    params["protocolVersion"] = "2024-11-05";
    params["capabilities"] = json::object();
    params["clientInfo"] = {
        {"name", client_name},
        {"version", client_version}
    };

    auto result = send_request("initialize", params);

    // Send initialized notification.
    send_notification("notifications/initialized");

    return result;
}

std::vector<json> McpStdioTransport::list_tools() {
    auto result = send_request("tools/list");
    std::vector<json> tools;
    if (result.contains("tools") && result["tools"].is_array()) {
        for (const auto& t : result["tools"]) {
            tools.push_back(t);
        }
    }
    return tools;
}

json McpStdioTransport::call_tool(const std::string& tool_name,
                                  const json& args) {
    json params;
    params["name"] = tool_name;
    params["arguments"] = args;
    return send_request("tools/call", params);
}

void McpStdioTransport::shutdown() {
    if (stdin_fd_ >= 0) {
        ::close(stdin_fd_);
        stdin_fd_ = -1;
    }
    if (stdout_fd_ >= 0) {
        ::close(stdout_fd_);
        stdout_fd_ = -1;
    }
    if (child_pid_ > 0) {
        ::kill(child_pid_, SIGTERM);
        // Give child a moment to exit gracefully, then force-kill.
        int status = 0;
        pid_t r = ::waitpid(child_pid_, &status, WNOHANG);
        if (r == 0) {
            // Still running — wait briefly then SIGKILL.
            usleep(200000);  // 200ms
            r = ::waitpid(child_pid_, &status, WNOHANG);
            if (r == 0) {
                ::kill(child_pid_, SIGKILL);
                ::waitpid(child_pid_, &status, 0);
            }
        }
        child_pid_ = -1;
    }
}

std::unordered_map<std::string, std::string> McpStdioTransport::build_child_env(
    const std::unordered_map<std::string, std::string>& server_env) {

    // Start with a safe subset of the current environment.
    static const std::array<const char*, 5> kSafeVars = {{
        "PATH", "HOME", "LANG", "SHELL", "TMPDIR"
    }};

    std::unordered_map<std::string, std::string> env;
    for (const auto* var : kSafeVars) {
        const char* val = std::getenv(var);
        if (val != nullptr) {
            env[var] = val;
        }
    }

    // Merge in server-specific env (overrides safe defaults).
    for (const auto& [k, v] : server_env) {
        env[k] = v;
    }

    // Apply the standard sensitive-var filter.
    return hermes::environments::filter_env(env);
}

#endif  // _WIN32

}  // namespace hermes::tools
