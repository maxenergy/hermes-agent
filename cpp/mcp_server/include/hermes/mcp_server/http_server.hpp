// HttpServer — Boost.Beast HTTP/1.1 server hosting the MCP endpoints:
//
//   GET  /sse                   — open a long-lived SSE stream; first frame
//                                 is an ``endpoint`` event pointing at
//                                 ``/messages?sessionId=<id>``.
//   POST /messages?sessionId=X  — push a JSON-RPC request onto session X;
//                                 response is the usual envelope, echoed
//                                 back on the SSE stream.
//   POST /                      — single-shot JSON-RPC request (no SSE).
//
// Thread model: one dedicated acceptor thread owning an ``io_context``.
// Each connection is handled synchronously on a worker thread pulled from
// an internal pool — keeps the code readable without pessimizing parallel
// request throughput.
#pragma once

#include "hermes/mcp_server/rpc_dispatch.hpp"
#include "hermes/mcp_server/session.hpp"

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace hermes::mcp_server {

class HttpServer {
public:
    struct Options {
        std::string bind_address = "127.0.0.1";
        // Use 0 to pick a random available port (resolvable via
        // ``listening_port()`` after ``start()``).
        std::uint16_t port = 0;
        std::size_t worker_threads = 2;
    };

    HttpServer(Options opts, SessionStore& sessions, RpcDispatcher& dispatcher);
    ~HttpServer();

    HttpServer(const HttpServer&) = delete;
    HttpServer& operator=(const HttpServer&) = delete;

    // Start the acceptor. Returns false on bind failure. Blocks briefly
    // until the socket is ready so callers can immediately query
    // ``listening_port()``.
    bool start();

    // Gracefully stop the server: close the listening socket, wake every
    // SSE waiter, and join worker threads.
    void stop();

    bool running() const { return running_.load(); }
    std::uint16_t listening_port() const { return listening_port_.load(); }

    // Counters for tests / observability.
    std::uint64_t total_requests() const { return total_requests_.load(); }
    std::uint64_t active_sse_streams() const { return active_sse_.load(); }

private:
    struct Impl;
    friend struct Impl;
    std::unique_ptr<Impl> impl_;
    Options opts_;
    SessionStore& sessions_;
    RpcDispatcher& dispatcher_;
    std::atomic<bool> running_{false};
    std::atomic<std::uint16_t> listening_port_{0};
    std::atomic<std::uint64_t> total_requests_{0};
    std::atomic<std::uint64_t> active_sse_{0};
};

}  // namespace hermes::mcp_server
