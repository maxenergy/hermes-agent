// Phase 9: Minimal single-shot WebSocket client (RFC 6455) over POSIX sockets.
//
// Only supports ws:// (no TLS), text frames, payloads < 64KB.
// Opens connection, performs HTTP upgrade, sends one masked text frame,
// reads one response frame, then closes.

#include "hermes/tools/simple_ws.hpp"

#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <random>
#include <sstream>
#include <string>
#include <vector>

namespace hermes::tools {

namespace {

/// RAII socket guard.
struct SocketGuard {
    int fd = -1;
    ~SocketGuard() {
        if (fd >= 0) ::close(fd);
    }
};

/// Parse a ws:// URL into host, port, path.
bool parse_ws_url(const std::string& url, std::string& host, int& port,
                  std::string& path) {
    // ws://host:port/path...
    const std::string prefix = "ws://";
    if (url.substr(0, prefix.size()) != prefix) return false;

    auto rest = url.substr(prefix.size());
    auto slash_pos = rest.find('/');
    std::string host_port;
    if (slash_pos == std::string::npos) {
        host_port = rest;
        path = "/";
    } else {
        host_port = rest.substr(0, slash_pos);
        path = rest.substr(slash_pos);
    }

    auto colon_pos = host_port.find(':');
    if (colon_pos == std::string::npos) {
        host = host_port;
        port = 80;
    } else {
        host = host_port.substr(0, colon_pos);
        port = std::stoi(host_port.substr(colon_pos + 1));
    }
    return !host.empty();
}

/// Generate 4 random masking bytes.
std::array<uint8_t, 4> random_mask() {
    static thread_local std::mt19937 rng(std::random_device{}());
    std::uniform_int_distribution<int> dist(0, 255);
    return {static_cast<uint8_t>(dist(rng)), static_cast<uint8_t>(dist(rng)),
            static_cast<uint8_t>(dist(rng)), static_cast<uint8_t>(dist(rng))};
}

/// Read exactly n bytes from fd.
bool read_exact(int fd, void* buf, size_t n, int timeout_sec) {
    auto* p = static_cast<uint8_t*>(buf);
    size_t total = 0;
    while (total < n) {
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(fd, &fds);
        struct timeval tv {};
        tv.tv_sec = timeout_sec;
        int sel = ::select(fd + 1, &fds, nullptr, nullptr, &tv);
        if (sel <= 0) return false;
        auto r = ::read(fd, p + total, n - total);
        if (r <= 0) return false;
        total += static_cast<size_t>(r);
    }
    return true;
}

/// Read bytes until `\r\n\r\n` is found (HTTP header terminator).
bool read_until_header_end(int fd, std::string& out, int timeout_sec) {
    out.clear();
    out.reserve(1024);
    while (true) {
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(fd, &fds);
        struct timeval tv {};
        tv.tv_sec = timeout_sec;
        int sel = ::select(fd + 1, &fds, nullptr, nullptr, &tv);
        if (sel <= 0) return false;
        char c;
        auto r = ::read(fd, &c, 1);
        if (r <= 0) return false;
        out.push_back(c);
        if (out.size() >= 4 && out.substr(out.size() - 4) == "\r\n\r\n")
            return true;
        if (out.size() > 16384) return false;  // sanity limit
    }
}

/// Base64-encode (for Sec-WebSocket-Key).
static const char b64_table[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string base64_encode(const uint8_t* data, size_t len) {
    std::string result;
    result.reserve(4 * ((len + 2) / 3));
    for (size_t i = 0; i < len; i += 3) {
        uint32_t n = static_cast<uint32_t>(data[i]) << 16;
        if (i + 1 < len) n |= static_cast<uint32_t>(data[i + 1]) << 8;
        if (i + 2 < len) n |= static_cast<uint32_t>(data[i + 2]);
        result.push_back(b64_table[(n >> 18) & 0x3F]);
        result.push_back(b64_table[(n >> 12) & 0x3F]);
        result.push_back((i + 1 < len) ? b64_table[(n >> 6) & 0x3F] : '=');
        result.push_back((i + 2 < len) ? b64_table[n & 0x3F] : '=');
    }
    return result;
}

}  // namespace

WsResponse ws_send_recv(const std::string& url, const std::string& message,
                        std::chrono::seconds timeout) {
    WsResponse resp;
    int timeout_sec = static_cast<int>(timeout.count());

    // 1. Parse URL
    std::string host;
    int port = 0;
    std::string path;
    if (!parse_ws_url(url, host, port, path)) {
        resp.error = "invalid ws:// URL: " + url;
        return resp;
    }

    // 2. DNS resolve
    struct addrinfo hints {};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    struct addrinfo* res = nullptr;
    if (::getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints,
                      &res) != 0 ||
        !res) {
        resp.error = "DNS resolution failed for " + host;
        return resp;
    }

    // 3. Connect
    SocketGuard sg;
    sg.fd = ::socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (sg.fd < 0) {
        ::freeaddrinfo(res);
        resp.error = "socket() failed";
        return resp;
    }

    // Set send/recv timeouts
    struct timeval tv {};
    tv.tv_sec = timeout_sec;
    ::setsockopt(sg.fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    ::setsockopt(sg.fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    if (::connect(sg.fd, res->ai_addr, res->ai_addrlen) < 0) {
        ::freeaddrinfo(res);
        resp.error = "connect() failed to " + host + ":" + std::to_string(port);
        return resp;
    }
    ::freeaddrinfo(res);

    // 4. WebSocket upgrade handshake
    auto mask_key = random_mask();
    std::string ws_key =
        base64_encode(mask_key.data(), mask_key.size());  // 8 chars, fine
    // Use a proper 16-byte nonce for Sec-WebSocket-Key
    std::array<uint8_t, 16> nonce{};
    {
        std::mt19937 rng(std::random_device{}());
        std::uniform_int_distribution<int> dist(0, 255);
        for (auto& b : nonce) b = static_cast<uint8_t>(dist(rng));
    }
    ws_key = base64_encode(nonce.data(), nonce.size());

    std::ostringstream req;
    req << "GET " << path << " HTTP/1.1\r\n"
        << "Host: " << host << ":" << port << "\r\n"
        << "Upgrade: websocket\r\n"
        << "Connection: Upgrade\r\n"
        << "Sec-WebSocket-Key: " << ws_key << "\r\n"
        << "Sec-WebSocket-Version: 13\r\n"
        << "\r\n";
    std::string req_str = req.str();

    if (::write(sg.fd, req_str.data(), req_str.size()) !=
        static_cast<ssize_t>(req_str.size())) {
        resp.error = "write() handshake failed";
        return resp;
    }

    // 5. Read 101 Switching Protocols response
    std::string header;
    if (!read_until_header_end(sg.fd, header, timeout_sec)) {
        resp.error = "failed to read WebSocket upgrade response";
        return resp;
    }
    if (header.find("101") == std::string::npos) {
        resp.error = "WebSocket upgrade rejected: " + header.substr(0, 80);
        return resp;
    }

    // 6. Send masked text frame
    {
        auto frame_mask = random_mask();
        size_t payload_len = message.size();
        std::vector<uint8_t> frame;
        // FIN + text opcode
        frame.push_back(0x81);
        // Mask bit set + length
        if (payload_len < 126) {
            frame.push_back(static_cast<uint8_t>(0x80 | payload_len));
        } else {
            frame.push_back(0x80 | 126);
            frame.push_back(static_cast<uint8_t>((payload_len >> 8) & 0xFF));
            frame.push_back(static_cast<uint8_t>(payload_len & 0xFF));
        }
        // Masking key
        frame.insert(frame.end(), frame_mask.begin(), frame_mask.end());
        // Masked payload
        for (size_t i = 0; i < payload_len; ++i) {
            frame.push_back(
                static_cast<uint8_t>(message[i]) ^ frame_mask[i % 4]);
        }

        if (::write(sg.fd, frame.data(), frame.size()) !=
            static_cast<ssize_t>(frame.size())) {
            resp.error = "write() frame failed";
            return resp;
        }
    }

    // 7. Read response frame
    {
        uint8_t hdr[2];
        if (!read_exact(sg.fd, hdr, 2, timeout_sec)) {
            resp.error = "failed to read response frame header";
            return resp;
        }
        // bool fin = (hdr[0] & 0x80) != 0;
        // uint8_t opcode = hdr[0] & 0x0F;
        bool masked = (hdr[1] & 0x80) != 0;
        uint64_t payload_len = hdr[1] & 0x7F;

        if (payload_len == 126) {
            uint8_t ext[2];
            if (!read_exact(sg.fd, ext, 2, timeout_sec)) {
                resp.error = "failed to read extended length";
                return resp;
            }
            payload_len =
                (static_cast<uint64_t>(ext[0]) << 8) | ext[1];
        } else if (payload_len == 127) {
            uint8_t ext[8];
            if (!read_exact(sg.fd, ext, 8, timeout_sec)) {
                resp.error = "failed to read extended length";
                return resp;
            }
            payload_len = 0;
            for (int i = 0; i < 8; ++i) {
                payload_len =
                    (payload_len << 8) | ext[i];
            }
        }

        std::array<uint8_t, 4> resp_mask{};
        if (masked) {
            if (!read_exact(sg.fd, resp_mask.data(), 4, timeout_sec)) {
                resp.error = "failed to read response mask";
                return resp;
            }
        }

        if (payload_len > 1024 * 1024) {
            resp.error = "response frame too large";
            return resp;
        }

        std::vector<uint8_t> payload(static_cast<size_t>(payload_len));
        if (payload_len > 0) {
            if (!read_exact(sg.fd, payload.data(),
                            static_cast<size_t>(payload_len), timeout_sec)) {
                resp.error = "failed to read response payload";
                return resp;
            }
        }

        if (masked) {
            for (size_t i = 0; i < payload.size(); ++i) {
                payload[i] ^= resp_mask[i % 4];
            }
        }

        resp.success = true;
        resp.data.assign(payload.begin(), payload.end());
    }

    // 8. Send close frame (best-effort)
    {
        uint8_t close_frame[] = {0x88, 0x80, 0x00, 0x00, 0x00, 0x00};
        (void)::write(sg.fd, close_frame, sizeof(close_frame));
    }

    return resp;
}

}  // namespace hermes::tools
