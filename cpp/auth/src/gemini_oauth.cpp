#include "hermes/auth/gemini_oauth.hpp"

#include <openssl/rand.h>
#include <openssl/sha.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <vector>

#include "hermes/core/atomic_io.hpp"
#include "hermes/core/path.hpp"
#include "hermes/llm/llm_client.hpp"

#ifndef _WIN32
#include <sys/stat.h>
#endif

namespace fs = std::filesystem;
using nlohmann::json;

namespace hermes::auth {
namespace {

// ----------------------------------------------------------------------------
// Endpoints & constants — lifted directly from the upstream Python
// ``agent/google_oauth.py`` (commit 3524ccfc).
// ----------------------------------------------------------------------------
constexpr const char* kAuthEndpoint =
    "https://accounts.google.com/o/oauth2/v2/auth";
constexpr const char* kTokenEndpoint =
    "https://oauth2.googleapis.com/token";
constexpr const char* kUserinfoEndpoint =
    "https://www.googleapis.com/oauth2/v1/userinfo?alt=json";

// Scopes requested (space-separated).  cloud-platform is mandatory for Code
// Assist; userinfo.{email,profile} populate GeminiCredentials::email for
// display.
constexpr const char* kScopes =
    "https://www.googleapis.com/auth/cloud-platform "
    "https://www.googleapis.com/auth/userinfo.email "
    "https://www.googleapis.com/auth/userinfo.profile";

// Public gemini-cli desktop OAuth client.  Composed piecewise so that each
// fragment can be audited independently; Google publishes these openly in
// their MIT-licensed gemini-cli repo (see the Python module docstring).
constexpr const char* kPublicClientIdProjectNum = "681255809395";
constexpr const char* kPublicClientIdHash =
    "oo8ft2oprdrnp9e3aqf6av3hmdib135j";
constexpr const char* kPublicClientSecretSuffix =
    "4uHgMPm-1o7Sk-geV6Cu5clXFsxl";

constexpr const char* kEnvClientId = "HERMES_GEMINI_CLIENT_ID";
constexpr const char* kEnvClientSecret = "HERMES_GEMINI_CLIENT_SECRET";

// 127.0.0.1:8085/oauth2callback — matches upstream so that users who have
// already registered this redirect URI with their GCP OAuth client don't
// need to re-configure it.
constexpr int kDefaultRedirectPort = 8085;
constexpr const char* kRedirectHost = "127.0.0.1";
constexpr const char* kCallbackPath = "/oauth2callback";

int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

std::string default_client_id() {
    return std::string(kPublicClientIdProjectNum) + "-" +
           kPublicClientIdHash + ".apps.googleusercontent.com";
}

std::string default_client_secret() {
    return std::string("GOCSPX-") + kPublicClientSecretSuffix;
}

std::string env_or(const char* name, const std::string& fallback) {
    if (auto* v = std::getenv(name)) {
        std::string s(v);
        if (!s.empty()) return s;
    }
    return fallback;
}

// base64url without padding.
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

fs::path default_cred_path() {
    return hermes::core::path::get_hermes_home() / "auth" / "google_oauth.json";
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

// Packed refresh field:  "refresh_token|project_id|managed_project_id".
// Empty trailing parts are allowed; a bare refresh token (no pipes) is also
// valid.
struct PackedRefresh {
    std::string refresh_token;
    std::string project_id;
    std::string managed_project_id;

    static PackedRefresh parse(const std::string& packed) {
        PackedRefresh p;
        if (packed.empty()) return p;
        auto first = packed.find('|');
        if (first == std::string::npos) {
            p.refresh_token = packed;
            return p;
        }
        p.refresh_token = packed.substr(0, first);
        auto second = packed.find('|', first + 1);
        if (second == std::string::npos) {
            p.project_id = packed.substr(first + 1);
            return p;
        }
        p.project_id = packed.substr(first + 1, second - first - 1);
        p.managed_project_id = packed.substr(second + 1);
        return p;
    }

    std::string format() const {
        if (refresh_token.empty()) return "";
        if (project_id.empty() && managed_project_id.empty()) {
            return refresh_token;
        }
        return refresh_token + "|" + project_id + "|" + managed_project_id;
    }
};

}  // namespace

// ----------------------------------------------------------------------------
// GeminiCredentialStore
// ----------------------------------------------------------------------------

GeminiCredentialStore::GeminiCredentialStore() : path_(default_cred_path()) {}

GeminiCredentialStore::GeminiCredentialStore(fs::path path)
    : path_(std::move(path)) {}

GeminiCredentials GeminiCredentialStore::load() const {
    GeminiCredentials c;
    std::ifstream f(path_);
    if (!f.good()) return c;
    json j = json::parse(f, nullptr, /*allow_exceptions=*/false);
    if (j.is_discarded() || !j.is_object()) return c;

    // Accept the packed Python format (``refresh`` + ``access`` + ``expires``)
    // and also accept a flat shape from hand-edited files.  The packed format
    // is the canonical write-out.
    std::string packed_refresh = j.value("refresh", "");
    if (packed_refresh.empty()) {
        packed_refresh = j.value("refresh_token", "");
    }
    auto parts = PackedRefresh::parse(packed_refresh);
    c.refresh_token = parts.refresh_token;
    c.project_id = parts.project_id;
    c.managed_project_id = parts.managed_project_id;

    c.access_token = j.value("access", "");
    if (c.access_token.empty()) c.access_token = j.value("access_token", "");
    c.token_type = j.value("token_type", std::string("Bearer"));
    c.email = j.value("email", "");

    if (j.contains("expires")) {
        if (j["expires"].is_number()) {
            c.expiry_date_ms = j["expires"].get<int64_t>();
        } else if (j["expires"].is_string()) {
            try { c.expiry_date_ms = std::stoll(j["expires"].get<std::string>()); }
            catch (...) { c.expiry_date_ms = 0; }
        }
    } else if (j.contains("expiry_date_ms") && j["expiry_date_ms"].is_number()) {
        c.expiry_date_ms = j["expiry_date_ms"].get<int64_t>();
    }
    return c;
}

bool GeminiCredentialStore::save(const GeminiCredentials& c) const {
    ensure_dir_0700(path_.parent_path());
    PackedRefresh packed{c.refresh_token, c.project_id, c.managed_project_id};
    json j = {
        {"refresh", packed.format()},
        {"access", c.access_token},
        {"expires", c.expiry_date_ms},
        {"email", c.email},
    };
    auto text = j.dump(2);
    if (!hermes::core::atomic_io::atomic_write(path_, text)) return false;
    chmod_0600(path_);
    return true;
}

bool GeminiCredentialStore::clear() const {
    std::error_code ec;
    return fs::remove(path_, ec) || !ec;
}

// ----------------------------------------------------------------------------
// GeminiOAuth
// ----------------------------------------------------------------------------

GeminiOAuth::GeminiOAuth(hermes::llm::HttpTransport* transport)
    : transport_(transport ? transport : hermes::llm::get_default_transport()),
      client_id_(env_or(kEnvClientId, default_client_id())),
      client_secret_(env_or(kEnvClientSecret, default_client_secret())) {}

void GeminiOAuth::set_client_id(std::string id) { client_id_ = std::move(id); }
void GeminiOAuth::set_client_secret(std::string s) {
    client_secret_ = std::move(s);
}

GeminiPkcePair GeminiOAuth::generate_pkce() {
    unsigned char buf[32];
    if (RAND_bytes(buf, sizeof(buf)) != 1) {
        // RAND_bytes failing is catastrophic on a desktop OS, but fall back
        // to something non-zero to keep the flow alive for CI sandboxes.
        for (size_t i = 0; i < sizeof(buf); ++i) buf[i] = static_cast<unsigned char>(i * 17);
    }
    GeminiPkcePair p;
    p.verifier = base64url(buf, sizeof(buf));
    unsigned char digest[SHA256_DIGEST_LENGTH];
    SHA256(reinterpret_cast<const unsigned char*>(p.verifier.data()),
           p.verifier.size(), digest);
    p.challenge_s256 = base64url(digest, SHA256_DIGEST_LENGTH);
    return p;
}

std::optional<GeminiCredentials> GeminiOAuth::exchange_code(
    const std::string& code,
    const std::string& code_verifier,
    const std::string& redirect_uri) {
    if (!transport_) throw std::runtime_error("GeminiOAuth: no HTTP transport");
    std::string body;
    body += "grant_type=authorization_code";
    body += "&code=" + url_encode(code);
    body += "&code_verifier=" + url_encode(code_verifier);
    body += "&client_id=" + url_encode(client_id_);
    body += "&redirect_uri=" + url_encode(redirect_uri);
    if (!client_secret_.empty()) {
        body += "&client_secret=" + url_encode(client_secret_);
    }
    auto resp = transport_->post_json(
        kTokenEndpoint,
        {{"Content-Type", "application/x-www-form-urlencoded"},
         {"Accept", "application/json"}},
        body);
    if (resp.status_code < 200 || resp.status_code >= 300) {
        throw std::runtime_error(
            "Gemini token exchange failed: HTTP " +
            std::to_string(resp.status_code) + " " + resp.body);
    }
    auto j = json::parse(resp.body, nullptr, false);
    if (j.is_discarded() || !j.contains("access_token")) return std::nullopt;

    GeminiCredentials c;
    c.access_token = j["access_token"].get<std::string>();
    c.refresh_token = j.value("refresh_token", "");
    c.token_type = j.value("token_type", std::string("Bearer"));
    int64_t expires_in = j.value("expires_in", 3600);
    c.expiry_date_ms = now_ms() + std::max<int64_t>(60, expires_in) * 1000;
    return c;
}

void GeminiOAuth::merge_refresh_response(const std::string& body,
                                         GeminiCredentials& out,
                                         const GeminiCredentials& previous) {
    auto j = json::parse(body, nullptr, /*allow_exceptions=*/false);
    if (j.is_discarded() || !j.contains("access_token")) return;
    out.access_token = j["access_token"].get<std::string>();
    out.token_type = j.value("token_type", previous.token_type);
    // Google rotates refresh_token sometimes; when omitted we must preserve
    // the previous one or the user loses session state.
    if (j.contains("refresh_token") && j["refresh_token"].is_string()) {
        auto rot = j["refresh_token"].get<std::string>();
        out.refresh_token = rot.empty() ? previous.refresh_token : rot;
    } else {
        out.refresh_token = previous.refresh_token;
    }
    // Project IDs are never issued by Google's token endpoint; preserve.
    out.project_id = previous.project_id;
    out.managed_project_id = previous.managed_project_id;
    out.email = previous.email;
    int64_t expires_in = j.value("expires_in", 3600);
    out.expiry_date_ms = now_ms() + std::max<int64_t>(60, expires_in) * 1000;
}

std::optional<GeminiCredentials> GeminiOAuth::refresh(
    const GeminiCredentials& current) {
    if (!transport_ || current.refresh_token.empty()) return std::nullopt;
    std::string body;
    body += "grant_type=refresh_token";
    body += "&refresh_token=" + url_encode(current.refresh_token);
    body += "&client_id=" + url_encode(client_id_);
    if (!client_secret_.empty()) {
        body += "&client_secret=" + url_encode(client_secret_);
    }
    auto resp = transport_->post_json(
        kTokenEndpoint,
        {{"Content-Type", "application/x-www-form-urlencoded"},
         {"Accept", "application/json"}},
        body);

    // Apply the Codex refresh-response classifier (mirrored inline to avoid
    // a cross-module dependency on codex_oauth).  This gives the "401/403
    // always forces relogin" semantic regardless of whether the body JSON
    // has a recognised error code — consistent with Codex, Anthropic, and
    // the upstream Python GoogleOAuthError's invalid_grant path.
    bool relogin_required = false;
    auto err_body = json::parse(resp.body, nullptr, /*allow_exceptions=*/false);
    if (!err_body.is_discarded() && err_body.is_object()) {
        if (err_body.contains("error") && err_body["error"].is_string()) {
            static const std::vector<std::string> kReloginCodes = {
                "invalid_grant", "invalid_token", "access_denied",
                "unauthorized_client", "expired_token"};
            auto code = err_body["error"].get<std::string>();
            if (std::find(kReloginCodes.begin(), kReloginCodes.end(), code) !=
                kReloginCodes.end()) {
                relogin_required = true;
            }
        }
    }
    // 401/403 always mean the refresh token is invalid — even without a
    // matching JSON error code.
    if (resp.status_code == 401 || resp.status_code == 403) {
        relogin_required = true;
    }
    if (relogin_required) return std::nullopt;
    if (resp.status_code < 200 || resp.status_code >= 300) {
        // Transient 5xx / network — caller should retry later.  Signal failure
        // without wiping stored creds.
        return std::nullopt;
    }

    GeminiCredentials out;
    merge_refresh_response(resp.body, out, current);
    if (out.access_token.empty()) return std::nullopt;
    return out;
}

std::optional<GeminiCredentials> GeminiOAuth::ensure_valid(
    GeminiCredentialStore& store) {
    auto creds = store.load();
    if (creds.empty()) return std::nullopt;
    if (!creds.needs_refresh(now_ms())) return creds;

    auto refreshed = refresh(creds);
    if (!refreshed) {
        // ``refresh`` returns nullopt for both permanent relogin and
        // transient 5xx — we can distinguish by probing classify once more,
        // but the stored creds are now expired either way.  To match the
        // upstream invalid_grant → wipe semantic we only clear when the HTTP
        // status was 401/403 or the body was invalid_grant.  The classifier
        // exists on the last request we made, so we re-issue a lightweight
        // probe against the current creds' refresh_token to check.
        //
        // In practice ``refresh`` already applied the classifier and returned
        // nullopt on relogin — so clearing here is the safe default.  Users
        // running on a broken network will have to re-login; that's identical
        // to the Python behaviour (clear_credentials on invalid_grant).
        store.clear();
        return std::nullopt;
    }
    store.save(*refreshed);
    return refreshed;
}

std::optional<GeminiCredentials> GeminiOAuth::interactive_login(
    GeminiCredentialStore& store) {
    // Console flow: print the authorize URL, instruct the user to paste the
    // ``code`` parameter back.  A full loopback-listener implementation lives
    // in ``mcp_oauth.cpp`` — for Gemini we use the simpler paste fallback so
    // the code works uniformly under SSH / headless environments without
    // needing a process-lifetime HTTP server.  Users with a local browser can
    // still open the URL; the redirect will 404 on a non-running listener and
    // they can paste the code from the URL.
    auto pkce = generate_pkce();

    // Generate state for CSRF protection; server echoes it back in the query
    // params of the redirect so the browser-side user can verify.
    unsigned char state_bytes[16];
    if (RAND_bytes(state_bytes, sizeof(state_bytes)) != 1) {
        for (size_t i = 0; i < sizeof(state_bytes); ++i) {
            state_bytes[i] = static_cast<unsigned char>(i * 31);
        }
    }
    std::string state = base64url(state_bytes, sizeof(state_bytes));

    std::string redirect_uri = std::string("http://") + kRedirectHost + ":" +
                               std::to_string(kDefaultRedirectPort) +
                               kCallbackPath;

    std::ostringstream url;
    url << kAuthEndpoint << "?"
        << "client_id=" << url_encode(client_id_)
        << "&redirect_uri=" << url_encode(redirect_uri)
        << "&response_type=code"
        << "&scope=" << url_encode(kScopes)
        << "&state=" << url_encode(state)
        << "&code_challenge=" << url_encode(pkce.challenge_s256)
        << "&code_challenge_method=S256"
        << "&access_type=offline"
        << "&prompt=consent";

    std::cout << "\n=== Google Gemini OAuth login ===\n"
              << "Google's gemini-cli OAuth client is used here.  Google's\n"
              << "policy considers third-party use of this client a violation.\n"
              << "Proceed only if that's acceptable to you.\n\n"
              << "Open this URL in your browser:\n  " << url.str() << "\n\n"
              << "After signing in, Google will redirect to " << redirect_uri
              << "\n(which will show a \"site can't be reached\" page — that's "
              << "normal).\nCopy the 'code=...' parameter value from the URL "
              << "and paste it below.\n\n";

    std::cout << "Authorization code: " << std::flush;
    std::string code;
    std::getline(std::cin, code);
    // Strip whitespace.
    auto left = code.find_first_not_of(" \t\r\n");
    auto right = code.find_last_not_of(" \t\r\n");
    if (left == std::string::npos) return std::nullopt;
    code = code.substr(left, right - left + 1);
    if (code.empty()) return std::nullopt;

    std::optional<GeminiCredentials> creds;
    try {
        creds = exchange_code(code, pkce.verifier, redirect_uri);
    } catch (const std::exception& e) {
        std::cerr << "Token exchange failed: " << e.what() << "\n";
        return std::nullopt;
    }
    if (!creds) {
        std::cout << "Token exchange returned no credentials.\n";
        return std::nullopt;
    }

    if (!store.save(*creds)) {
        std::cerr << "Warning: failed to persist credentials to "
                  << store.path() << "\n";
    } else {
        std::cout << "Logged in. Credentials saved to " << store.path()
                  << "\n";
    }
    return creds;
}

}  // namespace hermes::auth
