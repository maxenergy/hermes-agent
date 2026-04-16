// Boost.Beast HTTP/1.1 transport for the MCP server.
//
// Thread model: a single "acceptor" thread owns an ``asio::io_context`` and
// loops on ``acceptor_.accept()``. Each accepted socket is handled on a
// fresh detached worker thread (rate-limited by ``worker_threads``) so one
// long-lived SSE stream cannot starve new connections.
//
// The SSE path writes each queued frame as a Beast HTTP chunk; framing is
// synchronous and gated by ``SseQueue``'s condition variable. This keeps
// the code readable at the cost of some per-connection threads — fine for
// the expected workload (a few concurrent editor clients).
#include "hermes/mcp_server/http_server.hpp"

#include "hermes/mcp_server/rpc_types.hpp"

#include <boost/asio.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/version.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <exception>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace hermes::mcp_server {

namespace {
namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
using tcp = asio::ip::tcp;

std::string percent_decode(std::string_view in) {
    std::string out;
    out.reserve(in.size());
    for (std::size_t i = 0; i < in.size(); ++i) {
        char c = in[i];
        if (c == '+') {
            out.push_back(' ');
        } else if (c == '%' && i + 2 < in.size()) {
            auto hex = [](char ch) -> int {
                if (ch >= '0' && ch <= '9') return ch - '0';
                if (ch >= 'a' && ch <= 'f') return 10 + (ch - 'a');
                if (ch >= 'A' && ch <= 'F') return 10 + (ch - 'A');
                return -1;
            };
            int hi = hex(in[i + 1]), lo = hex(in[i + 2]);
            if (hi >= 0 && lo >= 0) {
                out.push_back(static_cast<char>((hi << 4) | lo));
                i += 2;
            } else {
                out.push_back(c);
            }
        } else {
            out.push_back(c);
        }
    }
    return out;
}

// Split ``/path?a=1&b=2`` into path + kv pairs.
std::pair<std::string, std::vector<std::pair<std::string, std::string>>>
split_target(std::string_view target) {
    std::vector<std::pair<std::string, std::string>> kv;
    auto q = target.find('?');
    std::string path(target.substr(0, q));
    if (q == std::string_view::npos) return {path, kv};
    auto qs = target.substr(q + 1);
    std::size_t start = 0;
    while (start <= qs.size()) {
        auto end = qs.find('&', start);
        auto segment =
            qs.substr(start, end == std::string_view::npos ? std::string_view::npos
                                                           : end - start);
        auto eq = segment.find('=');
        if (eq == std::string_view::npos) {
            kv.emplace_back(percent_decode(segment), "");
        } else {
            kv.emplace_back(percent_decode(segment.substr(0, eq)),
                            percent_decode(segment.substr(eq + 1)));
        }
        if (end == std::string_view::npos) break;
        start = end + 1;
    }
    return {path, kv};
}

std::string format_sse_frame(std::string_view event, std::string_view data) {
    std::string out;
    if (!event.empty()) {
        out.append("event: ");
        out.append(event);
        out.push_back('\n');
    }
    std::size_t start = 0;
    while (start <= data.size()) {
        auto nl = data.find('\n', start);
        auto slice =
            data.substr(start, nl == std::string_view::npos ? std::string_view::npos
                                                            : nl - start);
        out.append("data: ");
        out.append(slice);
        out.push_back('\n');
        if (nl == std::string_view::npos) break;
        start = nl + 1;
    }
    out.push_back('\n');
    return out;
}

void write_simple_response(tcp::socket& sock, http::status code,
                           const std::string& content_type,
                           const std::string& body) {
    http::response<http::string_body> res{code, 11};
    res.set(http::field::server, "hermes-mcp");
    res.set(http::field::content_type, content_type);
    res.body() = body;
    res.prepare_payload();
    boost::system::error_code ec;
    http::write(sock, res, ec);
    (void)ec;
}

}  // namespace

// ---------------------------------------------------------------------------
// HttpServer::Impl — owns the asio + accept/worker plumbing.
// ---------------------------------------------------------------------------

struct HttpServer::Impl {
    asio::io_context io;
    tcp::acceptor acceptor;
    std::thread accept_thread;
    std::mutex workers_mu;
    std::condition_variable worker_cv;
    std::atomic<std::size_t> active_workers{0};
    std::size_t worker_cap;

    Impl(std::size_t cap) : acceptor(io), worker_cap(cap == 0 ? 1 : cap) {}

    // --------------------------- connection handler ---------------------

    void handle_connection(HttpServer* self, tcp::socket sock) {
        try {
            beast::tcp_stream stream(std::move(sock));
            stream.expires_after(std::chrono::seconds(30));

            beast::flat_buffer buffer;
            http::request<http::string_body> req;
            boost::system::error_code ec;
            http::read(stream, buffer, req, ec);
            if (ec) return;

            stream.expires_never();  // SSE streams stay open indefinitely.

            auto [path, qs] = split_target(std::string(req.target()));
            auto& sock_ref = stream.socket();

            if (req.method() == http::verb::get && path == "/sse") {
                self->active_sse_.fetch_add(1);
                handle_get_sse(self, sock_ref, req);
                self->active_sse_.fetch_sub(1);
                return;
            }

            if (req.method() == http::verb::post && path == "/messages") {
                self->total_requests_.fetch_add(1);
                handle_post_messages(self, sock_ref, req, qs);
                return;
            }

            if (req.method() == http::verb::post && path == "/") {
                self->total_requests_.fetch_add(1);
                handle_post_root(self, sock_ref, req);
                return;
            }

            if (req.method() == http::verb::get && path == "/health") {
                write_simple_response(sock_ref, http::status::ok,
                                      "application/json",
                                      R"({"status":"ok"})");
                return;
            }

            write_simple_response(sock_ref, http::status::not_found,
                                  "application/json",
                                  R"({"error":"not_found"})");
        } catch (const std::exception&) {
            // One bad client must never crash the server.
        } catch (...) {
        }
    }

    void handle_post_messages(
        HttpServer* self, tcp::socket& sock,
        const http::request<http::string_body>& req,
        const std::vector<std::pair<std::string, std::string>>& qs) {
        std::string sid;
        for (const auto& kv : qs) {
            if (kv.first == "sessionId") { sid = kv.second; break; }
        }
        if (sid.empty()) {
            auto it = req.find("Mcp-Session-Id");
            if (it != req.end()) sid = std::string(it->value());
        }
        auto session = sid.empty() ? nullptr : self->sessions_.touch(sid);
        if (!sid.empty() && !session) {
            write_simple_response(sock, http::status::not_found,
                                  "application/json",
                                  R"({"error":"unknown_session"})");
            return;
        }

        auto response = self->dispatcher_.handle_raw(req.body(), session);
        if (session && session->queue && !response.is_null()) {
            session->queue->push(format_sse_frame("message", response.dump()));
        }

        if (response.is_null()) {
            http::response<http::string_body> res{http::status::accepted, 11};
            res.set(http::field::server, "hermes-mcp");
            res.set(http::field::content_type, "application/json");
            if (session) res.set("Mcp-Session-Id", session->id);
            res.body() = "";
            res.prepare_payload();
            boost::system::error_code ec;
            http::write(sock, res, ec);
            return;
        }

        http::response<http::string_body> res{http::status::ok, 11};
        res.set(http::field::server, "hermes-mcp");
        res.set(http::field::content_type, "application/json");
        if (session) res.set("Mcp-Session-Id", session->id);
        res.body() = response.dump();
        res.prepare_payload();
        boost::system::error_code ec;
        http::write(sock, res, ec);
    }

    void handle_post_root(HttpServer* self, tcp::socket& sock,
                          const http::request<http::string_body>& req) {
        std::shared_ptr<McpSession> session;
        auto it = req.find("Mcp-Session-Id");
        if (it != req.end()) {
            session = self->sessions_.touch(std::string(it->value()));
        }
        auto response = self->dispatcher_.handle_raw(req.body(), session);
        http::response<http::string_body> res{http::status::ok, 11};
        res.set(http::field::server, "hermes-mcp");
        res.set(http::field::content_type, "application/json");
        if (session) res.set("Mcp-Session-Id", session->id);
        if (response.is_null()) {
            res.result(http::status::accepted);
            res.body() = "";
        } else {
            res.body() = response.dump();
        }
        res.prepare_payload();
        boost::system::error_code ec;
        http::write(sock, res, ec);
    }

    void handle_get_sse(HttpServer* self, tcp::socket& sock,
                        const http::request<http::string_body>& req) {
        std::shared_ptr<McpSession> session;
        auto it = req.find("Mcp-Session-Id");
        if (it != req.end()) {
            session = self->sessions_.touch(std::string(it->value()));
        }
        if (!session) session = self->sessions_.create();

        http::response<http::empty_body> res{http::status::ok, 11};
        res.set(http::field::server, "hermes-mcp");
        res.set(http::field::content_type, "text/event-stream");
        res.set(http::field::cache_control,
                "no-cache, no-store, must-revalidate");
        res.set(http::field::connection, "keep-alive");
        res.set("Mcp-Session-Id", session->id);
        res.chunked(true);

        http::response_serializer<http::empty_body> sr{res};
        boost::system::error_code ec;
        http::write_header(sock, sr, ec);
        if (ec) return;

        auto write_chunk = [&](const std::string& body) {
            boost::system::error_code wec;
            asio::write(sock, http::make_chunk(asio::buffer(body)), wec);
            return !wec;
        };

        std::string first =
            format_sse_frame("endpoint", "/messages?sessionId=" + session->id);
        if (!write_chunk(first)) return;

        while (self->running() && !session->queue->is_shutdown()) {
            std::string out;
            if (session->queue->wait_and_pop(out,
                                              std::chrono::milliseconds(500))) {
                if (!write_chunk(out)) break;
            } else {
                if (!write_chunk(": keepalive\n\n")) break;
                self->sessions_.touch(session->id);
            }
        }

        boost::system::error_code wec;
        asio::write(sock, http::make_chunk_last(), wec);
    }
};

// ---------------------------------------------------------------------------
// HttpServer
// ---------------------------------------------------------------------------

HttpServer::HttpServer(Options opts, SessionStore& sessions,
                       RpcDispatcher& dispatcher)
    : impl_(std::make_unique<Impl>(opts.worker_threads)),
      opts_(std::move(opts)),
      sessions_(sessions),
      dispatcher_(dispatcher) {}

HttpServer::~HttpServer() { stop(); }

bool HttpServer::start() {
    if (running_.exchange(true)) return true;

    boost::system::error_code ec;
    asio::ip::address addr = asio::ip::make_address(opts_.bind_address, ec);
    if (ec) {
        running_.store(false);
        return false;
    }
    tcp::endpoint ep(addr, opts_.port);

    impl_->acceptor.open(ep.protocol(), ec);
    if (ec) {
        running_.store(false);
        return false;
    }
    impl_->acceptor.set_option(asio::socket_base::reuse_address(true), ec);
    impl_->acceptor.bind(ep, ec);
    if (ec) {
        impl_->acceptor.close();
        running_.store(false);
        return false;
    }
    impl_->acceptor.listen(asio::socket_base::max_listen_connections, ec);
    if (ec) {
        impl_->acceptor.close();
        running_.store(false);
        return false;
    }
    listening_port_.store(impl_->acceptor.local_endpoint().port());

    impl_->accept_thread = std::thread([this] {
        while (running_.load()) {
            boost::system::error_code aec;
            tcp::socket sock(impl_->io);
            impl_->acceptor.accept(sock, aec);
            if (aec) {
                if (!running_.load()) break;
                continue;
            }
            {
                std::unique_lock<std::mutex> lk(impl_->workers_mu);
                impl_->worker_cv.wait(lk, [this] {
                    return !running_.load() ||
                           impl_->active_workers.load() < impl_->worker_cap;
                });
                if (!running_.load()) break;
                impl_->active_workers.fetch_add(1);
            }

            std::thread t([this, s = std::move(sock)]() mutable {
                impl_->handle_connection(this, std::move(s));
                {
                    std::lock_guard<std::mutex> lk(impl_->workers_mu);
                    impl_->active_workers.fetch_sub(1);
                }
                impl_->worker_cv.notify_one();
            });
            t.detach();
        }
    });
    return true;
}

void HttpServer::stop() {
    if (!running_.exchange(false)) return;

    // On Linux, ``close()`` on another thread's accepting fd does NOT
    // reliably interrupt a blocking ``accept()`` — the syscall may return
    // EBADF but often just blocks until an actual connection arrives.
    // Wake the accept loop by kicking a self-connection.
    std::uint16_t port = listening_port_.load();
    boost::system::error_code ec;
    impl_->acceptor.close(ec);

    if (port != 0) {
        try {
            asio::io_context kick_io;
            tcp::socket kick(kick_io);
            kick.connect(
                tcp::endpoint(asio::ip::make_address("127.0.0.1"), port), ec);
            kick.close(ec);
        } catch (...) {
            // If we can't self-connect the thread still exits once a real
            // client arrives; join below has a watchdog.
        }
    }

    impl_->worker_cv.notify_all();

    // Wake every SSE stream so detached workers can return promptly.
    for (const auto& id : sessions_.list_ids()) {
        auto s = sessions_.get(id);
        if (s && s->queue) s->queue->shutdown();
    }

    if (impl_->accept_thread.joinable()) impl_->accept_thread.join();

    for (int i = 0; i < 200 && impl_->active_workers.load() > 0; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

}  // namespace hermes::mcp_server
