// Email platform adapter implementation.
#include "email.hpp"

#include <array>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <sstream>

#ifndef _WIN32
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#if defined(HERMES_HAVE_OPENSSL)
#include <openssl/err.h>
#include <openssl/ssl.h>
#endif

namespace hermes::gateway::platforms {

EmailAdapter::EmailAdapter(Config cfg) : cfg_(std::move(cfg)) {}

bool EmailAdapter::connect() {
    if (cfg_.address.empty() || cfg_.password.empty()) return false;
    // Receiving via IMAP IDLE requires a long-lived connection; send-only for now.
    return true;
}

void EmailAdapter::disconnect() {}

bool EmailAdapter::send(const std::string& chat_id,
                        const std::string& content) {
    // chat_id is the recipient email address.
    std::string mime = build_mime_message(cfg_.address, chat_id, "Hermes", content);

    // Try sendmail first (available on most Unix systems).
    std::string cmd = "sendmail -t 2>/dev/null";
    FILE* pipe = popen(cmd.c_str(), "w");
    if (!pipe) return false;

    std::fwrite(mime.data(), 1, mime.size(), pipe);
    int rc = pclose(pipe);
    return rc == 0;
}

void EmailAdapter::send_typing(const std::string& /*chat_id*/) {
    // Email has no typing indicator.
}

std::string EmailAdapter::imap_login_command(const std::string& tag,
                                             const std::string& user,
                                             const std::string& password) {
    return tag + " LOGIN " + user + " " + password + "\r\n";
}

std::string EmailAdapter::imap_select_command(const std::string& tag,
                                              const std::string& mailbox) {
    return tag + " SELECT " + mailbox + "\r\n";
}

std::string EmailAdapter::imap_idle_command(const std::string& tag) {
    return tag + " IDLE\r\n";
}

std::string EmailAdapter::imap_done_command() {
    return "DONE\r\n";
}

std::string EmailAdapter::imap_uid_fetch_command(const std::string& tag,
                                                 const std::string& uid_spec) {
    return tag + " UID FETCH " + uid_spec + " (RFC822)\r\n";
}

long long EmailAdapter::parse_exists_notification(const std::string& line) {
    // Expected: "* <n> EXISTS"
    if (line.size() < 4 || line[0] != '*' || line[1] != ' ') return -1;
    std::size_t i = 2;
    long long n = 0;
    bool have_digit = false;
    while (i < line.size() && line[i] >= '0' && line[i] <= '9') {
        n = n * 10 + (line[i] - '0');
        ++i;
        have_digit = true;
    }
    if (!have_digit) return -1;
    if (i >= line.size() || line[i] != ' ') return -1;
    ++i;
    // Require "EXISTS" (case-insensitive) as next token.
    std::string tok;
    while (i < line.size() && line[i] != '\r' && line[i] != '\n' &&
           line[i] != ' ') {
        tok += static_cast<char>(std::toupper(line[i]));
        ++i;
    }
    if (tok != "EXISTS") return -1;
    return n;
}

void EmailAdapter::start_imap_idle_loop(
    std::function<void(const std::string&)> message_handler) {
    if (idle_running_.exchange(true)) return;

    idle_thread_ = std::thread([this, handler = std::move(message_handler)]() {
        // Best-effort IDLE loop.  When OpenSSL / sockets are unavailable
        // at build time, this thread simply exits — tests that exercise
        // the loop gate on `IMAP_TEST_HOST`.
#if defined(HERMES_HAVE_OPENSSL) && !defined(_WIN32)
        while (idle_running_.load()) {
            int sock = ::socket(AF_INET, SOCK_STREAM, 0);
            if (sock < 0) { idle_running_.store(false); return; }
            struct addrinfo hints{}, *res = nullptr;
            hints.ai_family = AF_INET;
            hints.ai_socktype = SOCK_STREAM;
            if (::getaddrinfo(cfg_.imap_host.c_str(),
                              std::to_string(cfg_.imap_port).c_str(),
                              &hints, &res) != 0) {
                ::close(sock);
                idle_running_.store(false);
                return;
            }
            if (::connect(sock, res->ai_addr, res->ai_addrlen) != 0) {
                ::freeaddrinfo(res);
                ::close(sock);
                idle_running_.store(false);
                return;
            }
            ::freeaddrinfo(res);

            SSL_CTX* ctx = SSL_CTX_new(TLS_client_method());
            SSL* ssl = SSL_new(ctx);
            SSL_set_fd(ssl, sock);
            if (SSL_connect(ssl) != 1) {
                SSL_free(ssl);
                SSL_CTX_free(ctx);
                ::close(sock);
                idle_running_.store(false);
                return;
            }

            auto ssl_write = [&](const std::string& s) {
                SSL_write(ssl, s.data(), static_cast<int>(s.size()));
            };
            auto login = imap_login_command("a1", cfg_.address,
                                            cfg_.password);
            ssl_write(login);
            ssl_write(imap_select_command("a2", "INBOX"));
            ssl_write(imap_idle_command("a3"));

            char buf[4096];
            auto t0 = std::chrono::steady_clock::now();
            while (idle_running_.load()) {
                int n = SSL_read(ssl, buf, sizeof(buf) - 1);
                if (n <= 0) break;
                buf[n] = 0;
                std::istringstream lines(buf);
                std::string line;
                while (std::getline(lines, line)) {
                    if (parse_exists_notification(line) > 0) {
                        handler(line);
                    }
                }
                auto elapsed = std::chrono::steady_clock::now() - t0;
                if (elapsed > std::chrono::minutes(25)) {
                    ssl_write(imap_done_command());
                    ssl_write(imap_idle_command("a4"));
                    t0 = std::chrono::steady_clock::now();
                }
            }

            ssl_write(imap_done_command());
            SSL_shutdown(ssl);
            SSL_free(ssl);
            SSL_CTX_free(ctx);
            ::close(sock);
        }
#else
        (void)handler;
        idle_running_.store(false);
#endif
    });
}

void EmailAdapter::stop_imap_idle_loop() {
    idle_running_.store(false);
    if (idle_thread_.joinable()) idle_thread_.join();
}

std::string EmailAdapter::build_mime_message(const std::string& from,
                                             const std::string& to,
                                             const std::string& subject,
                                             const std::string& body) {
    std::string msg;
    msg += "From: " + from + "\r\n";
    msg += "To: " + to + "\r\n";
    msg += "Subject: " + subject + "\r\n";
    msg += "MIME-Version: 1.0\r\n";
    msg += "Content-Type: text/plain; charset=UTF-8\r\n";
    msg += "Content-Transfer-Encoding: 8bit\r\n";
    msg += "\r\n";
    msg += body;
    return msg;
}

}  // namespace hermes::gateway::platforms
