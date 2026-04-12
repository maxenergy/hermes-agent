#include "hermes/tools/mcp_transport.hpp"

#include "hermes/environments/env_filter.hpp"

#ifdef _WIN32
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
#include <sstream>
#include <stdexcept>

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

McpStdioTransport::McpStdioTransport(const std::string&,
                                     const std::vector<std::string>&,
                                     const std::unordered_map<std::string, std::string>&) {
    throw std::runtime_error("McpStdioTransport is not supported on Windows");
}
McpStdioTransport::~McpStdioTransport() = default;
bool McpStdioTransport::is_connected() const { return false; }
json McpStdioTransport::send_request(const std::string&, const json&, std::chrono::seconds) {
    return json{{"error", "not supported on Windows"}};
}
void McpStdioTransport::send_notification(const std::string&, const json&) {}
json McpStdioTransport::initialize(const std::string&, const std::string&) { return {}; }
std::vector<json> McpStdioTransport::list_tools() { return {}; }
json McpStdioTransport::call_tool(const std::string&, const json&) { return {}; }
void McpStdioTransport::shutdown() {}
void McpStdioTransport::write_message(const json&) {}
std::optional<json> McpStdioTransport::read_message(std::chrono::seconds) { return std::nullopt; }
std::unordered_map<std::string, std::string> McpStdioTransport::build_child_env(
    const std::unordered_map<std::string, std::string>&) { return {}; }

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
