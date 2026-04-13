#include "hermes/auth/mcp_oauth.hpp"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <openssl/evp.h>
#include <openssl/rand.h>

#include <array>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <random>
#include <sstream>
#include <thread>

#include <nlohmann/json.hpp>

#include "hermes/core/path.hpp"
#include "hermes/llm/llm_client.hpp"

namespace hermes::auth {

namespace {

using json = nlohmann::json;
namespace fs = std::filesystem;

// RFC 4648 base64url (no padding).
std::string base64url(const unsigned char* data, std::size_t len) {
    static const char* kAlphabet =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    std::string out;
    out.reserve(((len + 2) / 3) * 4);
    std::size_t i = 0;
    while (i + 3 <= len) {
        uint32_t n = (uint32_t(data[i]) << 16) |
                     (uint32_t(data[i + 1]) << 8) |
                     uint32_t(data[i + 2]);
        out.push_back(kAlphabet[(n >> 18) & 0x3F]);
        out.push_back(kAlphabet[(n >> 12) & 0x3F]);
        out.push_back(kAlphabet[(n >> 6) & 0x3F]);
        out.push_back(kAlphabet[n & 0x3F]);
        i += 3;
    }
    if (i < len) {
        uint32_t n = uint32_t(data[i]) << 16;
        if (i + 1 < len) n |= uint32_t(data[i + 1]) << 8;
        out.push_back(kAlphabet[(n >> 18) & 0x3F]);
        out.push_back(kAlphabet[(n >> 12) & 0x3F]);
        if (i + 1 < len) out.push_back(kAlphabet[(n >> 6) & 0x3F]);
    }
    return out;
}

std::string url_encode(const std::string& s) {
    static const char* kHex = "0123456789ABCDEF";
    std::string out;
    out.reserve(s.size());
    for (unsigned char c : s) {
        bool unreserved =
            (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') ||
            c == '-' || c == '_' || c == '.' || c == '~';
        if (unreserved) {
            out.push_back(static_cast<char>(c));
        } else {
            out.push_back('%');
            out.push_back(kHex[c >> 4]);
            out.push_back(kHex[c & 0x0F]);
        }
    }
    return out;
}

fs::path token_dir() {
    return hermes::core::path::get_hermes_home() / "mcp";
}

fs::path token_path(const std::string& server_name) {
    std::string safe;
    safe.reserve(server_name.size());
    for (char c : server_name) {
        if (c == '/' || c == '\\' || c == ':' || c == '\0') {
            safe.push_back('_');
        } else {
            safe.push_back(c);
        }
    }
    return token_dir() / (safe + ".token.json");
}

std::string url_join(const std::string& base, const std::string& rel) {
    if (rel.empty()) return base;
    if (rel.rfind("http://", 0) == 0 || rel.rfind("https://", 0) == 0) {
        return rel;
    }
    if (!base.empty() && base.back() == '/' && !rel.empty() && rel.front() == '/') {
        return base + rel.substr(1);
    }
    if (!base.empty() && base.back() != '/' && (rel.empty() || rel.front() != '/')) {
        return base + "/" + rel;
    }
    return base + rel;
}

}  // namespace

McpOAuth::McpOAuth(hermes::llm::HttpTransport* transport)
    : transport_(transport ? transport : hermes::llm::get_default_transport()) {}

McpOAuth::PkceChallenge McpOAuth::make_pkce() {
    PkceChallenge out;
    unsigned char buf[32];
    if (RAND_bytes(buf, sizeof(buf)) != 1) {
        // Fallback to std::random_device for test environments where OpenSSL
        // entropy may be limited.
        std::random_device rd;
        for (auto& b : buf) b = static_cast<unsigned char>(rd());
    }
    out.verifier = base64url(buf, sizeof(buf));

    // SHA-256 → base64url
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int dlen = 0;
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr);
    EVP_DigestUpdate(ctx, out.verifier.data(), out.verifier.size());
    EVP_DigestFinal_ex(ctx, digest, &dlen);
    EVP_MD_CTX_free(ctx);
    out.challenge = base64url(digest, dlen);
    out.method = "S256";
    return out;
}

std::string McpOAuth::build_authorize_url(const McpOAuthConfig& cfg,
                                          const PkceChallenge& pkce,
                                          const std::string& state) {
    std::string auth = cfg.authorization_url.empty()
                           ? url_join(cfg.server_url, "/authorize")
                           : cfg.authorization_url;
    std::ostringstream scope;
    for (std::size_t i = 0; i < cfg.scopes.size(); ++i) {
        if (i) scope << " ";
        scope << cfg.scopes[i];
    }
    std::ostringstream qs;
    qs << (auth.find('?') == std::string::npos ? "?" : "&")
       << "response_type=code"
       << "&client_id=" << url_encode(cfg.client_id)
       << "&redirect_uri=" << url_encode(cfg.redirect_uri)
       << "&code_challenge=" << url_encode(pkce.challenge)
       << "&code_challenge_method=" << pkce.method
       << "&state=" << url_encode(state);
    if (!cfg.scopes.empty()) {
        qs << "&scope=" << url_encode(scope.str());
    }
    return auth + qs.str();
}

int McpOAuth::extract_port(const std::string& redirect_uri) {
    auto scheme_end = redirect_uri.find("://");
    if (scheme_end == std::string::npos) return 0;
    auto rest = redirect_uri.substr(scheme_end + 3);
    auto slash = rest.find('/');
    auto host_port = rest.substr(0, slash);
    auto colon = host_port.rfind(':');
    if (colon == std::string::npos) return 0;
    try {
        return std::stoi(host_port.substr(colon + 1));
    } catch (...) {
        return 0;
    }
}

std::string McpOAuth::listen_for_code(int port, int timeout_sec,
                                      std::string* state_out) {
    int srv = ::socket(AF_INET, SOCK_STREAM, 0);
    if (srv < 0) return {};
    int opt = 1;
    ::setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(port));
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::bind(srv, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        ::close(srv);
        return {};
    }
    if (::listen(srv, 1) < 0) {
        ::close(srv);
        return {};
    }

    timeval tv{timeout_sec, 0};
    fd_set rfds;
    FD_ZERO(&rfds);
    FD_SET(srv, &rfds);
    int rc = ::select(srv + 1, &rfds, nullptr, nullptr, &tv);
    if (rc <= 0) {
        ::close(srv);
        return {};
    }

    sockaddr_in client{};
    socklen_t clen = sizeof(client);
    int cfd = ::accept(srv, reinterpret_cast<sockaddr*>(&client), &clen);
    ::close(srv);
    if (cfd < 0) return {};

    // Read the request line (up to the first CRLF CRLF).
    std::string buf;
    char chunk[4096];
    while (buf.size() < 65536) {
        ssize_t n = ::recv(cfd, chunk, sizeof(chunk), 0);
        if (n <= 0) break;
        buf.append(chunk, static_cast<size_t>(n));
        if (buf.find("\r\n\r\n") != std::string::npos) break;
    }

    // Extract code/state from "GET /callback?code=...&state=... HTTP/1.1"
    std::string code;
    std::string state_val;
    auto sp1 = buf.find(' ');
    if (sp1 != std::string::npos) {
        auto sp2 = buf.find(' ', sp1 + 1);
        if (sp2 != std::string::npos) {
            auto target = buf.substr(sp1 + 1, sp2 - sp1 - 1);
            auto qpos = target.find('?');
            if (qpos != std::string::npos) {
                auto qs = target.substr(qpos + 1);
                std::istringstream ss(qs);
                std::string kv;
                while (std::getline(ss, kv, '&')) {
                    auto eq = kv.find('=');
                    if (eq == std::string::npos) continue;
                    auto key = kv.substr(0, eq);
                    auto val = kv.substr(eq + 1);
                    if (key == "code") code = val;
                    else if (key == "state") state_val = val;
                }
            }
        }
    }

    const char* resp =
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/html; charset=utf-8\r\n"
        "Connection: close\r\n"
        "Content-Length: 96\r\n\r\n"
        "<html><body><h3>Hermes: authorization received.</h3>"
        "You can close this window.</body></html>";
    ::send(cfd, resp, std::strlen(resp), 0);
    ::close(cfd);

    if (state_out) *state_out = state_val;
    return code;
}

std::optional<McpOAuthToken> McpOAuth::exchange_code(
    const McpOAuthConfig& cfg,
    const std::string& code,
    const PkceChallenge& pkce) {
    if (!transport_) return std::nullopt;
    std::string tok_url = cfg.token_url.empty()
                              ? url_join(cfg.server_url, "/token")
                              : cfg.token_url;

    std::ostringstream body;
    body << "grant_type=authorization_code"
         << "&code=" << url_encode(code)
         << "&redirect_uri=" << url_encode(cfg.redirect_uri)
         << "&client_id=" << url_encode(cfg.client_id)
         << "&code_verifier=" << url_encode(pkce.verifier);
    if (!cfg.client_secret.empty()) {
        body << "&client_secret=" << url_encode(cfg.client_secret);
    }

    auto resp = transport_->post_json(
        tok_url,
        {{"Content-Type", "application/x-www-form-urlencoded"},
         {"Accept", "application/json"}},
        body.str());
    if (resp.status_code < 200 || resp.status_code >= 300) return std::nullopt;

    auto j = json::parse(resp.body, nullptr, false);
    if (j.is_discarded()) return std::nullopt;

    McpOAuthToken tok;
    tok.access_token = j.value("access_token", "");
    tok.refresh_token = j.value("refresh_token", "");
    tok.token_type = j.value("token_type", std::string("Bearer"));
    tok.scope = j.value("scope", "");
    int expires_in = j.value("expires_in", 0);
    tok.expiry_date_ms = expires_in > 0
                            ? std::chrono::duration_cast<std::chrono::milliseconds>(
                                  std::chrono::system_clock::now().time_since_epoch())
                                      .count() +
                                  int64_t(expires_in) * 1000
                            : 0;
    if (tok.access_token.empty()) return std::nullopt;
    return tok;
}

std::optional<McpOAuthToken> McpOAuth::interactive_login(
    const McpOAuthConfig& cfg) {
    if (!transport_) return std::nullopt;
    int port = extract_port(cfg.redirect_uri);
    if (port <= 0) return std::nullopt;

    auto pkce = make_pkce();
    unsigned char state_bytes[16];
    if (RAND_bytes(state_bytes, sizeof(state_bytes)) != 1) {
        std::random_device rd;
        for (auto& b : state_bytes) b = static_cast<unsigned char>(rd());
    }
    std::string state = base64url(state_bytes, sizeof(state_bytes));
    auto authorize_url = build_authorize_url(cfg, pkce, state);

    std::cout << "\nOpen the following URL in your browser to authorize:\n  "
              << authorize_url << "\n\nWaiting for callback on port " << port
              << "...\n";

    // Best-effort browser launch (xdg-open / open / start); failure is fine.
    std::string open_cmd;
#if defined(__APPLE__)
    open_cmd = "open '" + authorize_url + "' >/dev/null 2>&1 &";
#elif defined(_WIN32)
    open_cmd = "start \"\" \"" + authorize_url + "\"";
#else
    open_cmd = "xdg-open '" + authorize_url + "' >/dev/null 2>&1 &";
#endif
    int _rc = std::system(open_cmd.c_str());
    (void)_rc;

    std::string returned_state;
    std::string code = listen_for_code(port, 300, &returned_state);
    if (code.empty()) return std::nullopt;
    if (!returned_state.empty() && returned_state != state) return std::nullopt;

    return exchange_code(cfg, code, pkce);
}

std::optional<McpOAuthToken> McpOAuth::refresh(const McpOAuthConfig& cfg,
                                               const McpOAuthToken& current) {
    if (!transport_ || current.refresh_token.empty()) return std::nullopt;
    std::string tok_url = cfg.token_url.empty()
                              ? url_join(cfg.server_url, "/token")
                              : cfg.token_url;
    std::ostringstream body;
    body << "grant_type=refresh_token"
         << "&refresh_token=" << url_encode(current.refresh_token)
         << "&client_id=" << url_encode(cfg.client_id);
    if (!cfg.client_secret.empty()) {
        body << "&client_secret=" << url_encode(cfg.client_secret);
    }
    auto resp = transport_->post_json(
        tok_url,
        {{"Content-Type", "application/x-www-form-urlencoded"},
         {"Accept", "application/json"}},
        body.str());
    if (resp.status_code < 200 || resp.status_code >= 300) return std::nullopt;
    auto j = json::parse(resp.body, nullptr, false);
    if (j.is_discarded()) return std::nullopt;
    McpOAuthToken tok;
    tok.access_token = j.value("access_token", "");
    tok.refresh_token = j.value("refresh_token", current.refresh_token);
    tok.token_type = j.value("token_type", current.token_type);
    tok.scope = j.value("scope", current.scope);
    int expires_in = j.value("expires_in", 0);
    tok.expiry_date_ms = expires_in > 0
                            ? std::chrono::duration_cast<std::chrono::milliseconds>(
                                  std::chrono::system_clock::now().time_since_epoch())
                                      .count() +
                                  int64_t(expires_in) * 1000
                            : 0;
    if (tok.access_token.empty()) return std::nullopt;
    return tok;
}

bool McpOAuth::save_token(const std::string& server_name,
                          const McpOAuthToken& tok) {
    std::error_code ec;
    fs::create_directories(token_dir(), ec);
    auto p = token_path(server_name);
    nlohmann::json j;
    j["access_token"] = tok.access_token;
    j["refresh_token"] = tok.refresh_token;
    j["expiry_date_ms"] = tok.expiry_date_ms;
    j["token_type"] = tok.token_type;
    j["scope"] = tok.scope;
    std::ofstream ofs(p, std::ios::trunc);
    if (!ofs) return false;
    ofs << j.dump(2);
    ofs.close();
    ::chmod(p.c_str(), S_IRUSR | S_IWUSR);  // 0600
    return true;
}

std::optional<McpOAuthToken> McpOAuth::load_token(const std::string& server_name) {
    auto p = token_path(server_name);
    std::error_code ec;
    if (!fs::exists(p, ec)) return std::nullopt;
    std::ifstream ifs(p);
    if (!ifs) return std::nullopt;
    auto j = nlohmann::json::parse(ifs, nullptr, false);
    if (j.is_discarded()) return std::nullopt;
    McpOAuthToken tok;
    tok.access_token = j.value("access_token", "");
    tok.refresh_token = j.value("refresh_token", "");
    tok.expiry_date_ms = j.value("expiry_date_ms", int64_t{0});
    tok.token_type = j.value("token_type", std::string("Bearer"));
    tok.scope = j.value("scope", "");
    if (tok.access_token.empty()) return std::nullopt;
    return tok;
}

}  // namespace hermes::auth
