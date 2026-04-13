#include "hermes/core/uds_rpc.hpp"

#include "hermes/core/path.hpp"

#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <system_error>
#include <utility>

#ifdef _WIN32
// Windows named-pipe path — scaffolded but unused until the CLI/gateway
// spins up a Windows server.  Keeping the POSIX branch first so the
// mainstream Linux path stays simple.
#include <windows.h>
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>
#endif

namespace hermes::core::rpc {

namespace {

// ---------- Small socket helpers (POSIX only for now) -----------------

#ifndef _WIN32

bool set_cloexec(int fd) {
    int flags = ::fcntl(fd, F_GETFD, 0);
    if (flags < 0) return false;
    return ::fcntl(fd, F_SETFD, flags | FD_CLOEXEC) == 0;
}

ssize_t write_all(int fd, const char* buf, size_t len) {
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = ::send(fd, buf + sent, len - sent, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        sent += static_cast<size_t>(n);
    }
    return static_cast<ssize_t>(sent);
}

// Best-effort read into `out`, up to `cap` bytes, waiting at most
// `timeout_ms` total.  Returns bytes read, or -1 on error, or 0 on EOF
// when nothing was read.
ssize_t read_some(int fd, char* out, size_t cap, int timeout_ms) {
    struct pollfd p;
    p.fd = fd;
    p.events = POLLIN;
    p.revents = 0;
    int rc = ::poll(&p, 1, timeout_ms);
    if (rc <= 0) return rc;  // 0 = timeout, <0 = error
    if (p.revents & (POLLERR | POLLHUP | POLLNVAL)) {
        // Drain anyway
    }
    ssize_t n;
    do {
        n = ::recv(fd, out, cap, 0);
    } while (n < 0 && errno == EINTR);
    return n;
}

#endif  // !_WIN32

// Strip leading whitespace in place.
void lstrip(std::string& s) {
    size_t i = 0;
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i;
    if (i > 0) s.erase(0, i);
}

}  // namespace

// ---------- RpcError --------------------------------------------------

nlohmann::json RpcError::to_json() const {
    nlohmann::json j{{"code", code}, {"message", message}};
    if (!data.is_null()) j["data"] = data;
    return j;
}

// ---------- Framing ---------------------------------------------------

std::string frame_message(std::string_view json_payload) {
    std::string out;
    out.reserve(json_payload.size() + 32);
    out += "Content-Length: ";
    out += std::to_string(json_payload.size());
    out += "\r\n\r\n";
    out.append(json_payload);
    return out;
}

std::optional<std::string> try_parse_frame(std::string& buffer,
                                           std::size_t max_bytes) {
    // Find end-of-headers.
    auto header_end = buffer.find("\r\n\r\n");
    if (header_end == std::string::npos) {
        // Guard against clients that only send LF.
        auto lf_end = buffer.find("\n\n");
        if (lf_end == std::string::npos) return std::nullopt;
        header_end = lf_end;
    }
    // Parse headers.
    std::size_t content_length = 0;
    bool have_length = false;
    std::size_t pos = 0;
    while (pos < header_end) {
        auto eol = buffer.find_first_of("\r\n", pos);
        if (eol == std::string::npos || eol > header_end) eol = header_end;
        std::string line = buffer.substr(pos, eol - pos);
        pos = eol;
        // Skip CR/LF pair(s).
        while (pos < header_end &&
               (buffer[pos] == '\r' || buffer[pos] == '\n')) ++pos;
        if (line.empty()) continue;
        auto colon = line.find(':');
        if (colon == std::string::npos) continue;
        std::string key = line.substr(0, colon);
        std::string val = line.substr(colon + 1);
        lstrip(val);
        // Case-insensitive match on Content-Length.
        bool is_cl = (key.size() == 14);
        if (is_cl) {
            for (size_t i = 0; i < key.size(); ++i) {
                char c = static_cast<char>(std::tolower(
                    static_cast<unsigned char>(key[i])));
                static const char kExpected[] = "content-length";
                if (c != kExpected[i]) {
                    is_cl = false;
                    break;
                }
            }
        }
        if (is_cl) {
            try {
                content_length = static_cast<std::size_t>(std::stoull(val));
                have_length = true;
            } catch (...) {
                throw std::runtime_error(
                    "uds_rpc: malformed Content-Length header");
            }
        }
    }
    if (!have_length) {
        throw std::runtime_error("uds_rpc: missing Content-Length header");
    }
    if (content_length > max_bytes) {
        throw std::runtime_error("uds_rpc: frame exceeds max_bytes");
    }

    // Header-terminator length: the sequence we found (\r\n\r\n = 4,
    // or \n\n = 2).
    std::size_t term_len =
        (buffer.compare(header_end, 4, "\r\n\r\n") == 0) ? 4 : 2;
    std::size_t body_start = header_end + term_len;
    if (buffer.size() < body_start + content_length) {
        return std::nullopt;  // incomplete body
    }
    std::string payload = buffer.substr(body_start, content_length);
    buffer.erase(0, body_start + content_length);
    return payload;
}

// ---------- Socket-path resolution ------------------------------------

std::filesystem::path default_socket_path(std::string_view name) {
    auto home = hermes::core::path::get_hermes_home();
    auto run_dir = home / "run";
    std::error_code ec;
    std::filesystem::create_directories(run_dir, ec);
    std::string filename;
    filename.append(name.data(), name.size());
    filename += ".sock";
    return run_dir / filename;
}

// ---------- UdsServer -------------------------------------------------

UdsServer::UdsServer() = default;

UdsServer::~UdsServer() { stop(); }

void UdsServer::on(std::string method, MethodHandler handler) {
    std::lock_guard<std::mutex> lk(handlers_mu_);
    handlers_[std::move(method)] = std::move(handler);
}

#ifdef _WIN32

void UdsServer::start(const std::filesystem::path& socket_path) {
    // Windows scaffold — full implementation deferred.  Store the
    // path and throw at start so a Windows caller gets a clear signal
    // rather than an obscure socket error.  Named-pipe support will
    // follow in a later commit; until then we fail fast.
    socket_path_ = socket_path;
    throw std::runtime_error(
        "uds_rpc: Windows named-pipe server not yet implemented — "
        "use --enable-tcp-fallback or run on POSIX");
}

void UdsServer::stop() {
    running_.store(false);
    if (accept_thread_.joinable()) accept_thread_.join();
}

void UdsServer::accept_loop() {}
void UdsServer::handle_client(int) {}

#else

void UdsServer::start(const std::filesystem::path& socket_path) {
    if (running_.load()) {
        throw std::runtime_error("uds_rpc: server already running");
    }
    socket_path_ = socket_path;

    // If a stale socket file exists, try to unlink it.  A live listener
    // on the same path will surface as EADDRINUSE below.
    std::error_code ec;
    std::filesystem::remove(socket_path_, ec);

    int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        throw std::runtime_error(
            std::string("uds_rpc: socket() failed: ") + std::strerror(errno));
    }
    set_cloexec(fd);

    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    const std::string p = socket_path_.string();
    if (p.size() >= sizeof(addr.sun_path)) {
        ::close(fd);
        throw std::runtime_error("uds_rpc: socket path too long");
    }
    std::memcpy(addr.sun_path, p.c_str(), p.size() + 1);

    if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        int e = errno;
        ::close(fd);
        throw std::runtime_error(
            std::string("uds_rpc: bind failed: ") + std::strerror(e));
    }
    // Restrict perms: owner read/write only.
    ::chmod(p.c_str(), 0600);

    if (::listen(fd, 8) < 0) {
        int e = errno;
        ::close(fd);
        ::unlink(p.c_str());
        throw std::runtime_error(
            std::string("uds_rpc: listen failed: ") + std::strerror(e));
    }

    listen_fd_ = fd;
    running_.store(true);
    accept_thread_ = std::thread([this] { accept_loop(); });
}

void UdsServer::stop() {
    if (!running_.exchange(false)) {
        if (accept_thread_.joinable()) accept_thread_.join();
        return;
    }
    if (listen_fd_ >= 0) {
        ::shutdown(listen_fd_, SHUT_RDWR);
        ::close(listen_fd_);
        listen_fd_ = -1;
    }
    if (accept_thread_.joinable()) accept_thread_.join();
    if (!socket_path_.empty()) {
        ::unlink(socket_path_.c_str());
    }
}

void UdsServer::accept_loop() {
    while (running_.load()) {
        pollfd p{};
        p.fd = listen_fd_;
        p.events = POLLIN;
        int rc = ::poll(&p, 1, 250);
        if (rc <= 0) continue;
        sockaddr_un caddr{};
        socklen_t clen = sizeof(caddr);
        int cfd = ::accept(listen_fd_, reinterpret_cast<sockaddr*>(&caddr), &clen);
        if (cfd < 0) {
            if (errno == EINTR || errno == EAGAIN) continue;
            break;
        }
        set_cloexec(cfd);
        // Handle inline — small JSON-RPC traffic, keep it simple.
        // For concurrent clients a future patch can spawn a worker
        // pool.  Today the typical caller is a single in-process
        // component talking to a single CLI.
        handle_client(cfd);
    }
}

void UdsServer::handle_client(int client_fd) {
    std::string buffer;
    constexpr std::size_t kChunk = 8192;
    char tmp[kChunk];
    // Process as many framed requests as the client sends, until EOF
    // or error.
    while (running_.load()) {
        ssize_t n = read_some(client_fd, tmp, kChunk, 30000);
        if (n == 0) break;    // timeout
        if (n < 0) {
            if (errno == EAGAIN) continue;
            break;
        }
        buffer.append(tmp, static_cast<size_t>(n));
        while (true) {
            std::optional<std::string> payload;
            try {
                payload = try_parse_frame(buffer);
            } catch (const std::exception& ex) {
                // Framing error — respond best-effort then close.
                nlohmann::json err = {
                    {"jsonrpc", "2.0"},
                    {"id", nullptr},
                    {"error",
                     {{"code", -32700},
                      {"message", std::string("parse error: ") + ex.what()}}}};
                std::string resp = frame_message(err.dump());
                write_all(client_fd, resp.data(), resp.size());
                ::close(client_fd);
                return;
            }
            if (!payload) break;
            std::string resp_payload = process_payload(*payload);
            if (!resp_payload.empty()) {
                std::string framed = frame_message(resp_payload);
                if (write_all(client_fd, framed.data(), framed.size()) < 0) {
                    ::close(client_fd);
                    return;
                }
            }
        }
    }
    ::close(client_fd);
}

#endif  // _WIN32

std::string UdsServer::process_payload(std::string_view payload) {
    nlohmann::json req;
    try {
        req = nlohmann::json::parse(payload);
    } catch (const std::exception& ex) {
        nlohmann::json err = {
            {"jsonrpc", "2.0"},
            {"id", nullptr},
            {"error",
             {{"code", -32700},
              {"message", std::string("parse error: ") + ex.what()}}}};
        return err.dump();
    }
    if (!req.is_object() || req.value("jsonrpc", "") != "2.0") {
        nlohmann::json err = {
            {"jsonrpc", "2.0"},
            {"id", req.value("id", nlohmann::json(nullptr))},
            {"error",
             {{"code", -32600}, {"message", "invalid request"}}}};
        return err.dump();
    }
    std::string method = req.value("method", "");
    nlohmann::json params = req.value("params", nlohmann::json(nullptr));
    bool is_notification = !req.contains("id") || req["id"].is_null();
    nlohmann::json id = req.value("id", nlohmann::json(nullptr));

    MethodHandler h;
    {
        std::lock_guard<std::mutex> lk(handlers_mu_);
        auto it = handlers_.find(method);
        if (it != handlers_.end()) h = it->second;
    }

    if (!h) {
        if (is_notification) return {};
        nlohmann::json err = {
            {"jsonrpc", "2.0"},
            {"id", id},
            {"error",
             {{"code", -32601},
              {"message", std::string("method not found: ") + method}}}};
        return err.dump();
    }

    RpcResult result;
    try {
        result = h(params);
    } catch (const std::exception& ex) {
        result = RpcResult::fail(-32603, std::string("internal error: ") + ex.what());
    }

    if (is_notification) return {};

    if (result.has_error) {
        nlohmann::json err = {
            {"jsonrpc", "2.0"},
            {"id", id},
            {"error", result.error.to_json()}};
        return err.dump();
    }
    nlohmann::json ok = {
        {"jsonrpc", "2.0"},
        {"id", id},
        {"result", result.result}};
    return ok.dump();
}

// ---------- UdsClient -------------------------------------------------

UdsClient::UdsClient() = default;
UdsClient::~UdsClient() { close(); }

#ifdef _WIN32

void UdsClient::connect(const std::filesystem::path&, int) {
    throw std::runtime_error(
        "uds_rpc: Windows named-pipe client not yet implemented");
}
void UdsClient::close() { fd_ = -1; }

#else

void UdsClient::connect(const std::filesystem::path& socket_path,
                        int timeout_ms) {
    int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        throw std::runtime_error(
            std::string("uds_rpc: socket() failed: ") + std::strerror(errno));
    }
    set_cloexec(fd);

    // Non-blocking connect with poll() for timeout control.
    int flags = ::fcntl(fd, F_GETFL, 0);
    ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);

    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    const std::string p = socket_path.string();
    if (p.size() >= sizeof(addr.sun_path)) {
        ::close(fd);
        throw std::runtime_error("uds_rpc: socket path too long");
    }
    std::memcpy(addr.sun_path, p.c_str(), p.size() + 1);

    int rc = ::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    if (rc < 0 && errno != EINPROGRESS) {
        int e = errno;
        ::close(fd);
        throw std::runtime_error(
            std::string("uds_rpc: connect failed: ") + std::strerror(e));
    }
    if (rc < 0) {
        pollfd pf{};
        pf.fd = fd;
        pf.events = POLLOUT;
        int pr = ::poll(&pf, 1, timeout_ms);
        if (pr <= 0) {
            ::close(fd);
            throw std::runtime_error("uds_rpc: connect timed out");
        }
        int err = 0;
        socklen_t len = sizeof(err);
        ::getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len);
        if (err != 0) {
            ::close(fd);
            throw std::runtime_error(
                std::string("uds_rpc: connect failed: ") + std::strerror(err));
        }
    }
    // Restore blocking mode for simple request/response semantics.
    ::fcntl(fd, F_SETFL, flags);
    fd_ = fd;
}

void UdsClient::close() {
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
    rx_buffer_.clear();
}

#endif  // _WIN32

namespace {

#ifndef _WIN32

std::string recv_one_frame(int fd, std::string& buf, int timeout_ms) {
    constexpr std::size_t kChunk = 8192;
    char tmp[kChunk];
    while (true) {
        try {
            auto frame = try_parse_frame(buf);
            if (frame) return *frame;
        } catch (const std::exception&) {
            throw;
        }
        ssize_t n = read_some(fd, tmp, kChunk, timeout_ms);
        if (n == 0) throw std::runtime_error("uds_rpc: response timed out");
        if (n < 0) throw std::runtime_error("uds_rpc: read error");
        buf.append(tmp, static_cast<size_t>(n));
    }
}

#endif

}  // namespace

nlohmann::json UdsClient::call(std::string_view method,
                               const nlohmann::json& params,
                               int timeout_ms) {
#ifdef _WIN32
    (void)method;
    (void)params;
    (void)timeout_ms;
    throw std::runtime_error("uds_rpc: Windows client not yet implemented");
#else
    if (fd_ < 0) throw std::runtime_error("uds_rpc: not connected");
    std::int64_t id = next_id_++;
    nlohmann::json req = {
        {"jsonrpc", "2.0"},
        {"id", id},
        {"method", std::string(method)}};
    if (!params.is_null()) req["params"] = params;
    std::string framed = frame_message(req.dump());
    if (write_all(fd_, framed.data(), framed.size()) < 0) {
        throw std::runtime_error("uds_rpc: write failed");
    }
    std::string payload = recv_one_frame(fd_, rx_buffer_, timeout_ms);
    auto resp = nlohmann::json::parse(payload);
    if (resp.contains("error") && !resp["error"].is_null()) {
        std::string msg = resp["error"].value("message", "rpc error");
        int code = resp["error"].value("code", -32603);
        throw std::runtime_error("uds_rpc: " + msg + " (code=" +
                                 std::to_string(code) + ")");
    }
    return resp.value("result", nlohmann::json(nullptr));
#endif
}

void UdsClient::notify(std::string_view method,
                       const nlohmann::json& params) {
#ifdef _WIN32
    (void)method;
    (void)params;
    throw std::runtime_error("uds_rpc: Windows client not yet implemented");
#else
    if (fd_ < 0) throw std::runtime_error("uds_rpc: not connected");
    nlohmann::json req = {
        {"jsonrpc", "2.0"},
        {"method", std::string(method)}};
    if (!params.is_null()) req["params"] = params;
    std::string framed = frame_message(req.dump());
    if (write_all(fd_, framed.data(), framed.size()) < 0) {
        throw std::runtime_error("uds_rpc: write failed");
    }
#endif
}

}  // namespace hermes::core::rpc
