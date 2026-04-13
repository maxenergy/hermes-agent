// Unix Domain Socket JSON-RPC 2.0 server/client with Content-Length framing.
//
// Replaces file + subprocess IPC used by some hermes components.  On
// POSIX systems listens on ${HERMES_HOME}/run/<name>.sock with 0600
// permissions.  On Windows falls back to named pipes
// (\\.\pipe\hermes-<name>) or — if --enable-tcp-fallback — a localhost
// TCP loopback bound to 127.0.0.1 with a side-car port file.
//
// Framing protocol:
//   Content-Length: <N>\r\n
//   \r\n
//   <N bytes of UTF-8 JSON payload>
//
// Each JSON payload is a single JSON-RPC 2.0 Request, Notification or
// Response object.  The server dispatches by method name.
#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

#include <nlohmann/json.hpp>

namespace hermes::core::rpc {

// A parsed JSON-RPC 2.0 error.  `code` follows the spec ranges:
//   -32700 parse error, -32600 invalid request, -32601 method not found,
//   -32602 invalid params, -32603 internal error; -32000..-32099
//   reserved for implementation-defined server errors.
struct RpcError {
    int code = 0;
    std::string message;
    nlohmann::json data = nullptr;

    nlohmann::json to_json() const;
};

// Return type of a server-side method handler.  Returning `has_error`
// true causes the server to serialise an error response; otherwise the
// `result` json is wrapped in a normal JSON-RPC response.  For
// notifications (no id) the return value is ignored.
struct RpcResult {
    bool has_error = false;
    RpcError error;
    nlohmann::json result = nullptr;

    static RpcResult ok(nlohmann::json value) {
        RpcResult r;
        r.result = std::move(value);
        return r;
    }
    static RpcResult fail(int code, std::string message,
                          nlohmann::json data = nullptr) {
        RpcResult r;
        r.has_error = true;
        r.error = RpcError{code, std::move(message), std::move(data)};
        return r;
    }
};

using MethodHandler = std::function<RpcResult(const nlohmann::json& params)>;

// Framing helpers — public so UdsClient (or external callers) can reuse
// them over any bidirectional byte stream (stdio, TCP, etc).
std::string frame_message(std::string_view json_payload);

// Parse the next framed message from `buffer`.  On success, returns the
// JSON payload and erases it (with its headers) from the front of
// `buffer`.  On incomplete frame, returns std::nullopt and leaves the
// buffer untouched so the caller can wait for more bytes.  Throws
// std::runtime_error for malformed headers / oversized frames.
std::optional<std::string> try_parse_frame(std::string& buffer,
                                           std::size_t max_bytes = 32 * 1024 * 1024);

// Socket-path resolution.  Returns ${HERMES_HOME}/run/<name>.sock —
// creates the run dir on demand.  Name should be a short slug like
// "compressor" or "audio".
std::filesystem::path default_socket_path(std::string_view name);

class UdsServer {
public:
    UdsServer();
    ~UdsServer();

    UdsServer(const UdsServer&) = delete;
    UdsServer& operator=(const UdsServer&) = delete;

    // Register a method handler.  Safe to call before start().  Handlers
    // added after start() are visible to new requests (internal mutex).
    void on(std::string method, MethodHandler handler);

    // Bind + listen on the given socket path (POSIX) or pipe name
    // (Windows).  Throws std::runtime_error on failure.  Removes any
    // stale socket file that exists and is not connectable.
    void start(const std::filesystem::path& socket_path);

    // Stop the accept loop, close all client sockets, and join the
    // worker thread.  Safe to call multiple times.
    void stop();

    // Returns the bound socket path (or named-pipe/tcp descriptor).
    const std::filesystem::path& socket_path() const { return socket_path_; }

    // True while the accept loop is running.
    bool running() const { return running_.load(); }

private:
    void accept_loop();
    void handle_client(int client_fd);
    std::string process_payload(std::string_view payload);

    std::mutex handlers_mu_;
    std::unordered_map<std::string, MethodHandler> handlers_;

    std::filesystem::path socket_path_;
    int listen_fd_ = -1;
    std::atomic<bool> running_{false};
    std::thread accept_thread_;
};

// Client is synchronous: each call() sends a Request and blocks until
// the matching Response arrives (or timeout_ms elapses).  For
// concurrent use from multiple threads, each thread should own its own
// UdsClient (the client is cheap to construct).
class UdsClient {
public:
    UdsClient();
    ~UdsClient();

    UdsClient(const UdsClient&) = delete;
    UdsClient& operator=(const UdsClient&) = delete;

    // Connect.  Throws std::runtime_error on failure.
    void connect(const std::filesystem::path& socket_path,
                 int timeout_ms = 5000);

    // Close the underlying fd.  Safe to call multiple times.
    void close();

    // Fire a JSON-RPC request and wait for the response.  Generates a
    // fresh integer id.  Returns the parsed result on success, throws
    // std::runtime_error on transport error or returned RpcError.
    nlohmann::json call(std::string_view method,
                        const nlohmann::json& params = nullptr,
                        int timeout_ms = 30000);

    // Fire-and-forget notification (no id, no response expected).
    void notify(std::string_view method,
                const nlohmann::json& params = nullptr);

    bool connected() const { return fd_ >= 0; }

private:
    int fd_ = -1;
    std::int64_t next_id_ = 1;
    std::string rx_buffer_;
};

}  // namespace hermes::core::rpc
