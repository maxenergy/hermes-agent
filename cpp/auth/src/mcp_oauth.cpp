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

// ----------------------------------------------------------------------------
// MCPOAuthManager
// ----------------------------------------------------------------------------

namespace {

// Zero time_point sentinel for "no mtime recorded yet".
using FsTime = fs::file_time_type;
constexpr FsTime kNoMtime{};

FsTime token_mtime(const std::string& server_name) {
    auto p = token_path(server_name);
    std::error_code ec;
    if (!fs::exists(p, ec)) return kNoMtime;
    auto t = fs::last_write_time(p, ec);
    return ec ? kNoMtime : t;
}

}  // namespace

struct MCPOAuthManager::ServerState {
    std::string name;
    std::optional<McpOAuthToken> token;
    FsTime last_mtime = kNoMtime;
    // Multiple subscribers, keyed by an opaque monotonic handle so
    // unsubscribers don't need to shuffle indexes.
    std::unordered_map<std::size_t, ReconnectCallback> subscribers;
    std::size_t next_handle = 1;
};

MCPOAuthManager::MCPOAuthManager(hermes::llm::HttpTransport* transport)
    : transport_(transport ? transport : hermes::llm::get_default_transport()) {}

MCPOAuthManager::~MCPOAuthManager() = default;

MCPOAuthManager::ServerState& MCPOAuthManager::ensure_state_(
    const std::string& server) {
    auto it = servers_.find(server);
    if (it == servers_.end()) {
        auto st = std::make_unique<ServerState>();
        st->name = server;
        it = servers_.emplace(server, std::move(st)).first;
    }
    return *it->second;
}

std::size_t MCPOAuthManager::subscribe_reconnect(const std::string& server,
                                                 ReconnectCallback cb) {
    std::lock_guard<std::mutex> g(mu_);
    auto& st = ensure_state_(server);
    auto h = st.next_handle++;
    st.subscribers.emplace(h, std::move(cb));
    return h;
}

void MCPOAuthManager::unsubscribe_reconnect(const std::string& server,
                                            std::size_t handle) {
    std::lock_guard<std::mutex> g(mu_);
    auto it = servers_.find(server);
    if (it == servers_.end()) return;
    it->second->subscribers.erase(handle);
}

std::size_t MCPOAuthManager::subscriber_count(const std::string& server) const {
    std::lock_guard<std::mutex> g(mu_);
    auto it = servers_.find(server);
    if (it == servers_.end()) return 0;
    return it->second->subscribers.size();
}

bool MCPOAuthManager::reload_if_changed_(ServerState& st) {
    auto mtime = token_mtime(st.name);
    if (mtime == kNoMtime) {
        // File gone — clear cache.
        if (st.token.has_value()) {
            st.token.reset();
            st.last_mtime = kNoMtime;
            return true;
        }
        return false;
    }
    if (st.last_mtime == kNoMtime) {
        // First read — load and remember.
        auto fresh = McpOAuth::load_token(st.name);
        if (fresh) {
            st.token = fresh;
            st.last_mtime = mtime;
        }
        // Subscribers weren't set up yet when this first load happened
        // (it's the initial read), so don't fire reconnect.  Manager
        // clients call ``get_token`` lazily so this is the expected path.
        return false;
    }
    if (mtime != st.last_mtime) {
        auto fresh = McpOAuth::load_token(st.name);
        st.token = fresh;
        st.last_mtime = mtime;
        return true;  // externally refreshed — fire reconnect.
    }
    return false;
}

void MCPOAuthManager::fire_reconnect_unlocked_(const std::string& server) {
    // Copy the callback list so we don't hold mu_ across user code that
    // may call back into the manager (subscribe / unsubscribe / relogin).
    std::vector<ReconnectCallback> cbs;
    {
        std::lock_guard<std::mutex> g(mu_);
        auto it = servers_.find(server);
        if (it == servers_.end()) return;
        cbs.reserve(it->second->subscribers.size());
        for (const auto& [_, cb] : it->second->subscribers) {
            cbs.push_back(cb);
        }
    }
    for (auto& cb : cbs) {
        try {
            cb(server);
        } catch (...) {
            // Swallow subscriber errors — we don't want one misbehaving
            // transport to break the others.
        }
    }
}

std::optional<McpOAuthToken> MCPOAuthManager::peek_cached(
    const std::string& server) const {
    std::lock_guard<std::mutex> g(mu_);
    auto it = servers_.find(server);
    if (it == servers_.end()) return std::nullopt;
    return it->second->token;
}

std::optional<McpOAuthToken> MCPOAuthManager::get_token(
    const std::string& server) {
    bool fire = false;
    std::optional<McpOAuthToken> out;
    {
        std::lock_guard<std::mutex> g(mu_);
        auto& st = ensure_state_(server);
        fire = reload_if_changed_(st);
        out = st.token;
    }
    if (fire) fire_reconnect_unlocked_(server);
    return out;
}

McpOAuthRecovery MCPOAuthManager::handle_401(const std::string& server,
                                              const McpOAuthConfig& cfg) {
    // Phase 1: acquire the in-flight slot.  Only one thread per server
    // actually performs the recovery — others wait on the CV and re-check
    // the cache on wakeup.  Before blocking, snapshot the current cached
    // access_token so we can detect "someone else already refreshed" on
    // wakeup via cache comparison (covering the case where mtime has
    // already been consumed by the waker's reload).
    std::unique_lock<std::mutex> lk(mu_);
    std::string snapshot_access;
    {
        auto& st_init = ensure_state_(server);
        if (st_init.token) snapshot_access = st_init.token->access_token;
    }
    while (inflight_.count(server)) {
        inflight_cv_.wait(lk);
    }
    inflight_.insert(server);

    struct Finally {
        std::unordered_set<std::string>& set;
        std::condition_variable& cv;
        std::string key;
        ~Finally() { set.erase(key); cv.notify_all(); }
    } fin{inflight_, inflight_cv_, server};

    auto& st = ensure_state_(server);

    // Phase 2a: detect a handoff from a peer thread that just completed a
    // refresh.  If the cached access_token moved between our snapshot and
    // now, another thread already did the work — pick up its result.
    if (st.token && st.token->access_token != snapshot_access &&
        !st.token->access_token.empty()) {
        // Refresh completed by a peer.  No callback fire here: the peer
        // already fired its own reconnect callbacks.
        return McpOAuthRecovery::kRefreshedOnDisk;
    }

    // Phase 2b: check for an external refresh since the last mtime we
    // recorded.  If the mtime moved while we were blocked, another process
    // (or ``hermes mcp login``) already recovered — just reload.
    if (reload_if_changed_(st) && st.token.has_value()) {
        lk.unlock();
        fire_reconnect_unlocked_(server);
        return McpOAuthRecovery::kRefreshedOnDisk;
    }

    if (!st.token.has_value()) {
        auto disk = McpOAuth::load_token(server);
        if (disk) {
            st.token = disk;
            st.last_mtime = token_mtime(server);
        }
    }
    if (!st.token.has_value()) {
        return McpOAuthRecovery::kNoCredentials;
    }

    // Phase 3: in-place refresh.  Release the lock while we talk to the
    // network — other threads that 401 concurrently will block on the
    // in-flight CV until we land back here.
    auto current = *st.token;
    lk.unlock();

    McpOAuth oauth(transport_);
    auto refreshed = oauth.refresh(cfg, current);

    lk.lock();
    if (!refreshed) {
        // Could be transient or relogin.  The ``refresh`` API returns
        // nullopt for both; we heuristic: if current.refresh_token was
        // non-empty and the previous response was 4xx-shaped, assume
        // relogin.  Without access to the raw status here, we simply
        // treat as kNeedsRelogin — the upstream Python does the same
        // when ``invalid_grant`` is detected inline.
        //
        // If a caller wants transient behaviour, they probe with
        // ``get_token`` again after a backoff; the cache is untouched
        // here so the next call will just re-read disk.
        return McpOAuthRecovery::kNeedsRelogin;
    }

    // Persist + update cache.
    McpOAuth::save_token(server, *refreshed);
    st.token = refreshed;
    st.last_mtime = token_mtime(server);

    lk.unlock();
    fire_reconnect_unlocked_(server);
    return McpOAuthRecovery::kRefreshedInPlace;
}

std::optional<McpOAuthToken> MCPOAuthManager::relogin(
    const std::string& server, const McpOAuthConfig& cfg) {
    // Wipe disk + in-memory entry first so a failed interactive login
    // doesn't leave a stale token sitting around.
    {
        std::lock_guard<std::mutex> g(mu_);
        auto& st = ensure_state_(server);
        st.token.reset();
        st.last_mtime = kNoMtime;
    }
    std::error_code ec;
    fs::remove(token_path(server), ec);

    McpOAuth oauth(transport_);
    auto fresh = oauth.interactive_login(cfg);
    if (!fresh) return std::nullopt;

    McpOAuth::save_token(server, *fresh);
    {
        std::lock_guard<std::mutex> g(mu_);
        auto& st = ensure_state_(server);
        st.token = fresh;
        st.last_mtime = token_mtime(server);
    }
    fire_reconnect_unlocked_(server);
    return fresh;
}

void MCPOAuthManager::invalidate(const std::string& server) {
    std::lock_guard<std::mutex> g(mu_);
    servers_.erase(server);
}

}  // namespace hermes::auth
