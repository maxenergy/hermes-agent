#include "hermes/auth/qwen_oauth.hpp"

#include <openssl/evp.h>
#include <openssl/rand.h>
#include <openssl/sha.h>
#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <thread>

#include "hermes/core/atomic_io.hpp"
#include "hermes/llm/llm_client.hpp"

#ifndef _WIN32
#include <sys/stat.h>
#endif

namespace fs = std::filesystem;
using nlohmann::json;

namespace hermes::auth {
namespace {

// ----------------------------------------------------------------------------
// Constants — pulled directly from qwen-code (Apache-2.0):
// packages/core/src/qwen/qwenOAuth2.ts
// ----------------------------------------------------------------------------
constexpr const char* kOAuthBase = "https://chat.qwen.ai";
constexpr const char* kDeviceCodeEndpoint =
    "https://chat.qwen.ai/api/v1/oauth2/device/code";
constexpr const char* kTokenEndpoint =
    "https://chat.qwen.ai/api/v1/oauth2/token";
constexpr const char* kClientId = "f0304373b74a44d2b584a3fb70ca9e56";
constexpr const char* kScope = "openid profile email model.completion";
constexpr const char* kGrantTypeDeviceCode =
    "urn:ietf:params:oauth:grant-type:device_code";
constexpr const char* kDefaultDashscopeBase =
    "https://dashscope.aliyuncs.com/compatible-mode/v1";

int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

// base64url (RFC 4648 §5) without padding.
std::string base64url(const unsigned char* data, size_t len) {
    static const char kAlphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    std::string out;
    out.reserve((len + 2) / 3 * 4);
    for (size_t i = 0; i < len; i += 3) {
        uint32_t v = static_cast<uint32_t>(data[i]) << 16;
        if (i + 1 < len) v |= static_cast<uint32_t>(data[i + 1]) << 8;
        if (i + 2 < len) v |= static_cast<uint32_t>(data[i + 2]);
        out.push_back(kAlphabet[(v >> 18) & 0x3F]);
        out.push_back(kAlphabet[(v >> 12) & 0x3F]);
        if (i + 1 < len) out.push_back(kAlphabet[(v >> 6) & 0x3F]);
        if (i + 2 < len) out.push_back(kAlphabet[v & 0x3F]);
    }
    return out;
}

std::string url_encode(const std::string& s) {
    std::ostringstream o;
    o << std::hex << std::uppercase;
    for (unsigned char c : s) {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' ||
            c == '~') {
            o << static_cast<char>(c);
        } else {
            o << '%';
            if (c < 0x10) o << '0';
            o << static_cast<int>(c);
        }
    }
    return o.str();
}

std::string form_encode(std::initializer_list<std::pair<std::string, std::string>> pairs) {
    std::string out;
    bool first = true;
    for (auto& [k, v] : pairs) {
        if (!first) out += '&';
        first = false;
        out += url_encode(k) + "=" + url_encode(v);
    }
    return out;
}

fs::path qwen_dir() {
    if (auto* h = std::getenv("HOME")) return fs::path(h) / ".qwen";
    return fs::current_path() / ".qwen";
}

void ensure_dir_0700(const fs::path& p) {
    std::error_code ec;
    fs::create_directories(p, ec);
#ifndef _WIN32
    ::chmod(p.c_str(), S_IRWXU);
#endif
}

void chmod_0600(const fs::path& p) {
#ifndef _WIN32
    ::chmod(p.c_str(), S_IRUSR | S_IWUSR);
#else
    (void)p;
#endif
}

}  // namespace

// ----------------------------------------------------------------------------
// QwenCredentialStore
// ----------------------------------------------------------------------------

QwenCredentialStore::QwenCredentialStore()
    : path_(qwen_dir() / "oauth_creds.json") {}

QwenCredentialStore::QwenCredentialStore(fs::path path) : path_(std::move(path)) {}

QwenCredentials QwenCredentialStore::load() const {
    QwenCredentials c;
    std::ifstream f(path_);
    if (!f.good()) return c;
    try {
        json j;
        f >> j;
        c.access_token = j.value("access_token", "");
        c.refresh_token = j.value("refresh_token", "");
        c.token_type = j.value("token_type", "Bearer");
        c.resource_url = j.value("resource_url", "");
        if (j.contains("expiry_date")) {
            // qwen-code stores as number (epoch ms) or string.
            if (j["expiry_date"].is_number()) {
                c.expiry_date_ms = j["expiry_date"].get<int64_t>();
            } else if (j["expiry_date"].is_string()) {
                try { c.expiry_date_ms = std::stoll(j["expiry_date"].get<std::string>()); }
                catch (...) { c.expiry_date_ms = 0; }
            }
        }
    } catch (const std::exception&) {
        return {};
    }
    return c;
}

bool QwenCredentialStore::save(const QwenCredentials& c) const {
    ensure_dir_0700(path_.parent_path());
    json j = {
        {"access_token", c.access_token},
        {"token_type", c.token_type},
        {"refresh_token", c.refresh_token},
        {"resource_url", c.resource_url},
        {"expiry_date", c.expiry_date_ms},
    };
    auto text = j.dump();
    if (!hermes::core::atomic_io::atomic_write(path_, text)) return false;
    chmod_0600(path_);
    return true;
}

bool QwenCredentialStore::clear() const {
    std::error_code ec;
    return fs::remove(path_, ec) || !ec;
}

// ----------------------------------------------------------------------------
// QwenOAuth
// ----------------------------------------------------------------------------

QwenOAuth::QwenOAuth(hermes::llm::HttpTransport* transport)
    : transport_(transport ? transport : hermes::llm::get_default_transport()) {}

QwenOAuth::PkcePair QwenOAuth::generate_pkce() {
    unsigned char buf[32];
    RAND_bytes(buf, sizeof(buf));
    PkcePair p;
    p.verifier = base64url(buf, sizeof(buf));

    unsigned char digest[SHA256_DIGEST_LENGTH];
    SHA256(reinterpret_cast<const unsigned char*>(p.verifier.data()),
           p.verifier.size(), digest);
    p.challenge_s256 = base64url(digest, SHA256_DIGEST_LENGTH);
    return p;
}

QwenDeviceCode QwenOAuth::request_device_code(const std::string& code_challenge_s256) {
    if (!transport_) throw std::runtime_error("QwenOAuth: no HTTP transport");
    auto body = form_encode({
        {"client_id", kClientId},
        {"scope", kScope},
        {"code_challenge", code_challenge_s256},
        {"code_challenge_method", "S256"},
    });
    auto resp = transport_->post_json(
        kDeviceCodeEndpoint,
        {{"Content-Type", "application/x-www-form-urlencoded"},
         {"Accept", "application/json"}},
        body);
    if (resp.status_code < 200 || resp.status_code >= 300) {
        throw std::runtime_error(
            "Qwen device_code request failed: HTTP " +
            std::to_string(resp.status_code) + " " + resp.body);
    }
    auto j = json::parse(resp.body);
    if (!j.contains("device_code")) {
        throw std::runtime_error("Qwen device_code response missing device_code: " +
                                 resp.body);
    }
    QwenDeviceCode d;
    d.device_code = j["device_code"].get<std::string>();
    d.user_code = j.value("user_code", "");
    d.verification_uri = j.value("verification_uri",
                                  std::string(kOAuthBase) + "/authorize");
    d.verification_uri_complete = j.value("verification_uri_complete",
                                           d.verification_uri);
    d.expires_in = j.value("expires_in", 1800);
    return d;
}

std::optional<QwenCredentials> QwenOAuth::poll_for_token(
    const std::string& device_code,
    const std::string& code_verifier,
    std::chrono::seconds interval,
    std::chrono::seconds max_wait) {
    if (!transport_) throw std::runtime_error("QwenOAuth: no HTTP transport");

    auto deadline = std::chrono::steady_clock::now() + max_wait;
    auto sleep_for = interval;

    while (std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(sleep_for);

        auto body = form_encode({
            {"grant_type", kGrantTypeDeviceCode},
            {"client_id", kClientId},
            {"device_code", device_code},
            {"code_verifier", code_verifier},
        });
        auto resp = transport_->post_json(
            kTokenEndpoint,
            {{"Content-Type", "application/x-www-form-urlencoded"},
             {"Accept", "application/json"}},
            body);

        // 2xx = token issued.
        if (resp.status_code >= 200 && resp.status_code < 300) {
            auto j = json::parse(resp.body, nullptr, false);
            if (j.is_discarded() || !j.contains("access_token") ||
                j["access_token"].is_null()) {
                continue;
            }
            QwenCredentials c;
            c.access_token = j["access_token"].get<std::string>();
            c.refresh_token = j.value("refresh_token", "");
            c.token_type = j.value("token_type", "Bearer");
            c.resource_url = j.value("resource_url", "");
            int64_t expires_in = j.value("expires_in", 3600);
            c.expiry_date_ms = now_ms() + expires_in * 1000;
            return c;
        }

        // 400 + authorization_pending = keep polling.
        // 429 + slow_down = back off.
        auto j = json::parse(resp.body, nullptr, false);
        if (!j.is_discarded()) {
            std::string err = j.value("error", "");
            if (resp.status_code == 400 && err == "authorization_pending") {
                continue;
            }
            if (resp.status_code == 429 && err == "slow_down") {
                sleep_for += std::chrono::seconds(5);
                continue;
            }
            // expired_token / access_denied / etc. -> abort.
            if (err == "expired_token" || err == "access_denied" ||
                err == "invalid_grant") {
                return std::nullopt;
            }
        }
        // Anything else: give up.
        return std::nullopt;
    }
    return std::nullopt;
}

std::optional<QwenCredentials> QwenOAuth::refresh(const QwenCredentials& current) {
    if (!transport_ || current.refresh_token.empty()) return std::nullopt;

    auto body = form_encode({
        {"grant_type", "refresh_token"},
        {"refresh_token", current.refresh_token},
        {"client_id", kClientId},
    });
    auto resp = transport_->post_json(
        kTokenEndpoint,
        {{"Content-Type", "application/x-www-form-urlencoded"},
         {"Accept", "application/json"}},
        body);

    if (resp.status_code < 200 || resp.status_code >= 300) {
        // 400 indicates refresh token revoked / expired.
        return std::nullopt;
    }

    auto j = json::parse(resp.body, nullptr, false);
    if (j.is_discarded() || !j.contains("access_token")) return std::nullopt;

    QwenCredentials c = current;
    c.access_token = j["access_token"].get<std::string>();
    c.token_type = j.value("token_type", current.token_type);
    if (j.contains("refresh_token") && j["refresh_token"].is_string()) {
        c.refresh_token = j["refresh_token"].get<std::string>();
    }
    if (j.contains("resource_url") && j["resource_url"].is_string()) {
        c.resource_url = j["resource_url"].get<std::string>();
    }
    int64_t expires_in = j.value("expires_in", 3600);
    c.expiry_date_ms = now_ms() + expires_in * 1000;
    return c;
}

std::optional<QwenCredentials> QwenOAuth::interactive_login(QwenCredentialStore& store) {
    auto pkce = generate_pkce();

    QwenDeviceCode dc;
    try {
        dc = request_device_code(pkce.challenge_s256);
    } catch (const std::exception& e) {
        std::cerr << "Qwen device authorization failed: " << e.what() << "\n";
        return std::nullopt;
    }

    std::cout << "\n=== Qwen OAuth login ===\n"
              << "Open this URL in your browser:\n"
              << "  " << dc.verification_uri_complete << "\n";
    if (!dc.user_code.empty() &&
        dc.verification_uri_complete.find(dc.user_code) == std::string::npos) {
        std::cout << "Enter the code: " << dc.user_code << "\n";
    }
    std::cout << "\nWaiting for authorization (up to "
              << (dc.expires_in / 60) << " minutes)...\n";

    auto creds = poll_for_token(dc.device_code, pkce.verifier);
    if (!creds) {
        std::cout << "Authorization failed or timed out.\n";
        return std::nullopt;
    }

    if (!store.save(*creds)) {
        std::cerr << "Warning: failed to persist credentials to "
                  << store.path() << "\n";
    } else {
        std::cout << "Logged in. Credentials saved to " << store.path() << "\n";
    }
    return creds;
}

std::optional<QwenCredentials> QwenOAuth::ensure_valid(QwenCredentialStore& store) {
    auto creds = store.load();
    if (creds.empty()) return std::nullopt;

    if (!creds.needs_refresh(now_ms())) return creds;

    auto refreshed = refresh(creds);
    if (!refreshed) {
        // Refresh failed permanently — clear stored creds so caller knows to
        // re-run the device flow.
        store.clear();
        return std::nullopt;
    }
    store.save(*refreshed);
    return refreshed;
}

// ----------------------------------------------------------------------------
// API base URL resolution
// ----------------------------------------------------------------------------
std::string qwen_api_base_url(const QwenCredentials& creds) {
    std::string base = creds.resource_url.empty() ? kDefaultDashscopeBase
                                                   : creds.resource_url;
    if (base.compare(0, 7, "http://") != 0 &&
        base.compare(0, 8, "https://") != 0) {
        base = "https://" + base;
    }
    // Strip trailing slash.
    while (!base.empty() && base.back() == '/') base.pop_back();
    // Append /v1 if missing (matches qwen-code behaviour).
    if (base.size() < 3 || base.substr(base.size() - 3) != "/v1") {
        base += "/v1";
    }
    return base;
}

}  // namespace hermes::auth
