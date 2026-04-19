// Verifies that CurlTransport enables TCP keepalive on provider HTTP
// connections (mirror of Python #10324 / 8c478983).
//
// Spins up a tiny HTTP/1.1 server on loopback, drives a real
// ``CurlTransport`` against it, and then inspects the kernel view via
// ``getsockopt`` (captured inside the curl SOCKOPTFUNCTION hook — by the
// time we return control the socket is already torn down).

#include "hermes/llm/llm_client.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <gtest/gtest.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <thread>

namespace {

// Small, request-at-a-time HTTP/1.1 server.  Accepts one connection,
// reads until headers end, writes a fixed 200 OK response, closes.
struct MiniHttpServer {
    std::atomic<bool> stop{false};
    int port = 0;
    std::thread th;

    ~MiniHttpServer() { shutdown(); }

    bool start() {
        int srv = ::socket(AF_INET, SOCK_STREAM, 0);
        if (srv < 0) return false;
        int one = 1;
        ::setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;
        if (::bind(srv, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) <
            0) {
            ::close(srv);
            return false;
        }
        socklen_t alen = sizeof(addr);
        if (::getsockname(srv, reinterpret_cast<sockaddr*>(&addr), &alen) <
            0) {
            ::close(srv);
            return false;
        }
        port = ntohs(addr.sin_port);
        if (::listen(srv, 4) < 0) {
            ::close(srv);
            return false;
        }
        th = std::thread([this, srv] { run_loop(srv); });
        return true;
    }

    void shutdown() {
        stop.store(true);
        if (th.joinable()) {
            int kick = ::socket(AF_INET, SOCK_STREAM, 0);
            if (kick >= 0) {
                sockaddr_in addr{};
                addr.sin_family = AF_INET;
                addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
                addr.sin_port = htons(port);
                (void)::connect(kick, reinterpret_cast<sockaddr*>(&addr),
                                 sizeof(addr));
                ::close(kick);
            }
            th.join();
        }
    }

    void run_loop(int srv) {
        while (!stop.load()) {
            sockaddr_in peer{};
            socklen_t plen = sizeof(peer);
            int cli = ::accept(srv, reinterpret_cast<sockaddr*>(&peer),
                                 &plen);
            if (cli < 0) continue;
            if (stop.load()) {
                ::close(cli);
                break;
            }
            handle_one(cli);
            ::close(cli);
        }
        ::close(srv);
    }

    void handle_one(int fd) {
        std::string req;
        char buf[1024];
        while (req.size() < 8192) {
            auto n = ::read(fd, buf, sizeof(buf));
            if (n <= 0) return;
            req.append(buf, static_cast<size_t>(n));
            if (req.find("\r\n\r\n") != std::string::npos) break;
        }
        static const char resp[] =
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: application/json\r\n"
            "Content-Length: 16\r\n"
            "Connection: close\r\n"
            "\r\n"
            "{\"ok\":true,\"n\":1}";
        (void)::write(fd, resp, sizeof(resp) - 1);
    }
};

}  // namespace

using hermes::llm::HttpTransport;
using hermes::llm::LastCurlSocketOptions;
using hermes::llm::last_curl_socket_options;
using hermes::llm::make_curl_transport;
using hermes::llm::reset_last_curl_socket_options;

class TcpKeepaliveTest : public ::testing::Test {
protected:
    void SetUp() override {
        reset_last_curl_socket_options();
        transport_ = make_curl_transport();
        ASSERT_NE(transport_, nullptr);
        ASSERT_TRUE(server_.start()) << "failed to start mini HTTP server";
    }

    void TearDown() override {
        server_.shutdown();
    }

    std::string url(const std::string& path) {
        return "http://127.0.0.1:" + std::to_string(server_.port) + path;
    }

    MiniHttpServer server_;
    std::unique_ptr<HttpTransport> transport_;
};

// POST against the loopback server, then confirm the sockopt hook fired
// and the kernel view of the socket matches the configured keepalives.
TEST_F(TcpKeepaliveTest, PostEnablesKeepalive) {
    auto resp = transport_->post_json(
        url("/echo"), {{"Content-Type", "application/json"}},
        R"({"ping":1})");
    EXPECT_EQ(resp.status_code, 200);

    auto obs = last_curl_socket_options();
    ASSERT_TRUE(obs.populated)
        << "SOCKOPTFUNCTION hook did not fire — keepalives not applied";

    EXPECT_EQ(obs.so_keepalive, 1) << "SO_KEEPALIVE not set";
#if defined(TCP_KEEPIDLE) || defined(TCP_KEEPALIVE)
    EXPECT_EQ(obs.tcp_keepidle, 30)
        << "TCP_KEEPIDLE / TCP_KEEPALIVE not set to 30s";
#endif
#if defined(TCP_KEEPINTVL)
    EXPECT_EQ(obs.tcp_keepintvl, 10)
        << "TCP_KEEPINTVL not set to 10s";
#endif
#if defined(TCP_KEEPCNT)
    EXPECT_EQ(obs.tcp_keepcnt, 3) << "TCP_KEEPCNT not set to 3";
#endif
}

// GET against the loopback server — same invariants.
TEST_F(TcpKeepaliveTest, GetEnablesKeepalive) {
    auto resp =
        transport_->get(url("/echo"), {{"Accept", "application/json"}});
    EXPECT_EQ(resp.status_code, 200);

    auto obs = last_curl_socket_options();
    ASSERT_TRUE(obs.populated);
    EXPECT_EQ(obs.so_keepalive, 1);
#if defined(TCP_KEEPIDLE) || defined(TCP_KEEPALIVE)
    EXPECT_EQ(obs.tcp_keepidle, 30);
#endif
#if defined(TCP_KEEPINTVL)
    EXPECT_EQ(obs.tcp_keepintvl, 10);
#endif
#if defined(TCP_KEEPCNT)
    EXPECT_EQ(obs.tcp_keepcnt, 3);
#endif
}

// reset_last_curl_socket_options() wipes the observation.
TEST(TcpKeepaliveReset, ClearsObservation) {
    // Populate via manual call path first so reset has something to wipe.
    reset_last_curl_socket_options();
    auto before = last_curl_socket_options();
    EXPECT_FALSE(before.populated);

    // Fake it by starting a server + call; we don't reuse the fixture so
    // the reset scope is obvious.
    MiniHttpServer srv;
    ASSERT_TRUE(srv.start());
    auto tp = make_curl_transport();
    (void)tp->get(
        "http://127.0.0.1:" + std::to_string(srv.port) + "/x", {});
    auto populated = last_curl_socket_options();
    EXPECT_TRUE(populated.populated);

    reset_last_curl_socket_options();
    auto wiped = last_curl_socket_options();
    EXPECT_FALSE(wiped.populated);
    srv.shutdown();
}
