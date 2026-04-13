// WebSocket transport implementations: a Boost.Beast-backed TLS client
// for production and an in-process mock for unit tests.
//
// The Beast implementation is compiled only when HERMES_GATEWAY_HAS_BEAST
// is defined (Boost.Beast headers present). The mock is always built.
#include "websocket_transport.hpp"

#include <atomic>
#include <deque>
#include <mutex>
#include <string>
#include <thread>

#if __has_include(<boost/beast/core.hpp>) && \
    __has_include(<boost/beast/websocket.hpp>) && \
    __has_include(<boost/beast/ssl.hpp>)
#define HERMES_GATEWAY_HAS_BEAST 1
#include <boost/asio/connect.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl/context.hpp>
#include <boost/asio/ssl/host_name_verification.hpp>
#include <boost/asio/ssl/stream.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/beast/websocket/ssl.hpp>
#endif

namespace hermes::gateway::platforms {

namespace {

// ------------------------------------------------------------------
// Mock transport — queues frames, no network.
// ------------------------------------------------------------------
class MockWebSocketTransport : public WebSocketTransport {
public:
    bool connect(const std::string& host, const std::string& port,
                 const std::string& path) override {
        (void)port;
        last_host_ = host;
        last_path_ = path;
        open_.store(true);
        return true;
    }

    void close() override {
        open_.store(false);
        if (close_cb_) close_cb_(1000, "normal");
    }

    bool send_text(const std::string& payload) override {
        if (!open_.load()) return false;
        std::lock_guard<std::mutex> lk(mu_);
        sent_.push_back(payload);
        return true;
    }

    void set_message_callback(MessageCallback cb) override {
        message_cb_ = std::move(cb);
    }

    void set_close_callback(CloseCallback cb) override {
        close_cb_ = std::move(cb);
    }

    bool is_open() const override { return open_.load(); }

    bool poll_one() override {
        if (!open_.load()) return false;
        std::string msg;
        {
            std::lock_guard<std::mutex> lk(mu_);
            if (inbound_.empty()) return true;  // still open, just empty
            msg = std::move(inbound_.front());
            inbound_.pop_front();
        }
        if (message_cb_) message_cb_(msg);
        return open_.load();
    }

    // Test helpers (exposed via friend access in tests that include this
    // header directly — downcast acceptable for test-only use).
    void inject_inbound(std::string frame) {
        std::lock_guard<std::mutex> lk(mu_);
        inbound_.push_back(std::move(frame));
    }

    std::vector<std::string> drain_sent() {
        std::lock_guard<std::mutex> lk(mu_);
        std::vector<std::string> out(sent_.begin(), sent_.end());
        sent_.clear();
        return out;
    }

    const std::string& last_host() const { return last_host_; }
    const std::string& last_path() const { return last_path_; }

private:
    std::atomic<bool> open_{false};
    std::mutex mu_;
    std::deque<std::string> inbound_;
    std::deque<std::string> sent_;
    MessageCallback message_cb_;
    CloseCallback close_cb_;
    std::string last_host_;
    std::string last_path_;
};

#ifdef HERMES_GATEWAY_HAS_BEAST
// ------------------------------------------------------------------
// Beast TLS WebSocket. Blocking reads on a dedicated thread; writes
// are synchronized on a mutex. TLS cert verification is always on and
// uses the system CA bundle.
// ------------------------------------------------------------------
namespace beast = boost::beast;
namespace http = beast::http;
namespace asio = boost::asio;
namespace ssl = asio::ssl;
using tcp = asio::ip::tcp;

class BeastWebSocketTransport : public WebSocketTransport {
public:
    BeastWebSocketTransport()
        : ctx_(ssl::context::tls_client) {
        ctx_.set_default_verify_paths();
        ctx_.set_verify_mode(ssl::verify_peer);
    }

    ~BeastWebSocketTransport() override { close(); }

    bool connect(const std::string& host, const std::string& port,
                 const std::string& path) override {
        try {
            io_ = std::make_unique<asio::io_context>();
            ws_ = std::make_unique<beast::websocket::stream<
                beast::ssl_stream<tcp::socket>>>(*io_, ctx_);

            // Enable SNI — required by most modern WSS hosts.
            if (!SSL_set_tlsext_host_name(ws_->next_layer().native_handle(),
                                          host.c_str())) {
                return false;
            }
            ws_->next_layer().set_verify_callback(
                ssl::host_name_verification(host));

            tcp::resolver resolver(*io_);
            auto results = resolver.resolve(host, port);
            auto ep = asio::connect(
                beast::get_lowest_layer(*ws_), results);
            std::string host_port = host + ':' + std::to_string(ep.port());

            ws_->next_layer().handshake(ssl::stream_base::client);
            ws_->handshake(host_port, path);

            open_.store(true);
            host_ = host;
            return true;
        } catch (...) {
            io_.reset();
            ws_.reset();
            return false;
        }
    }

    void close() override {
        if (!open_.exchange(false)) return;
        try {
            if (ws_) ws_->close(beast::websocket::close_code::normal);
        } catch (...) {
        }
        if (close_cb_) close_cb_(1000, "normal");
    }

    bool send_text(const std::string& payload) override {
        if (!open_.load() || !ws_) return false;
        std::lock_guard<std::mutex> lk(write_mu_);
        try {
            ws_->text(true);
            ws_->write(asio::buffer(payload));
            return true;
        } catch (...) {
            open_.store(false);
            return false;
        }
    }

    void set_message_callback(MessageCallback cb) override {
        message_cb_ = std::move(cb);
    }

    void set_close_callback(CloseCallback cb) override {
        close_cb_ = std::move(cb);
    }

    bool is_open() const override { return open_.load(); }

    bool poll_one() override {
        if (!open_.load() || !ws_) return false;
        try {
            beast::flat_buffer buf;
            ws_->read(buf);
            std::string msg = beast::buffers_to_string(buf.data());
            if (message_cb_) message_cb_(msg);
            return true;
        } catch (...) {
            open_.store(false);
            if (close_cb_) close_cb_(1006, "read-error");
            return false;
        }
    }

private:
    ssl::context ctx_;
    std::unique_ptr<asio::io_context> io_;
    std::unique_ptr<beast::websocket::stream<
        beast::ssl_stream<tcp::socket>>> ws_;
    std::atomic<bool> open_{false};
    std::mutex write_mu_;
    std::string host_;
    MessageCallback message_cb_;
    CloseCallback close_cb_;
};
#endif  // HERMES_GATEWAY_HAS_BEAST

}  // namespace

std::unique_ptr<WebSocketTransport> make_mock_websocket_transport() {
    return std::make_unique<MockWebSocketTransport>();
}

#ifdef HERMES_GATEWAY_HAS_BEAST
std::unique_ptr<WebSocketTransport> make_beast_websocket_transport() {
    return std::make_unique<BeastWebSocketTransport>();
}
#else
std::unique_ptr<WebSocketTransport> make_beast_websocket_transport() {
    return nullptr;  // Beast not available in this build
}
#endif

// Expose a downcastable mock pointer for tests that need injection. The
// mock class is file-local, so tests use the helper below rather than
// including the class definition. Tests declare extern these helpers.
MockWebSocketTransport* as_mock(WebSocketTransport* t) {
    return dynamic_cast<MockWebSocketTransport*>(t);
}

void mock_inject_inbound(WebSocketTransport* t, std::string frame) {
    if (auto* m = as_mock(t)) m->inject_inbound(std::move(frame));
}

std::vector<std::string> mock_drain_sent(WebSocketTransport* t) {
    if (auto* m = as_mock(t)) return m->drain_sent();
    return {};
}

std::string mock_last_host(WebSocketTransport* t) {
    if (auto* m = as_mock(t)) return m->last_host();
    return {};
}

std::string mock_last_path(WebSocketTransport* t) {
    if (auto* m = as_mock(t)) return m->last_path();
    return {};
}

}  // namespace hermes::gateway::platforms
