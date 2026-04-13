// Qwen Code OAuth — device code flow with PKCE.
// Endpoints:  https://chat.qwen.ai/api/v1/oauth2/{device/code,token}
// Storage:    ~/.qwen/oauth_creds.json (compatible with qwen-code CLI)
// Default API base: https://dashscope.aliyuncs.com/compatible-mode/v1
//
// The credentials file is shared with the qwen-code Node.js CLI — once the
// user logs in via either tool, the other can use the token immediately.
#pragma once

#include <chrono>
#include <filesystem>
#include <optional>
#include <string>

namespace hermes::llm {
class HttpTransport;
}

namespace hermes::auth {

struct QwenDeviceCode {
    std::string device_code;
    std::string user_code;
    std::string verification_uri;
    std::string verification_uri_complete;  // pre-filled URL
    int expires_in = 1800;
};

struct QwenCredentials {
    std::string access_token;
    std::string refresh_token;
    std::string token_type = "Bearer";
    std::string resource_url;        // host suffix like "portal.qwen.ai"
    int64_t expiry_date_ms = 0;      // epoch milliseconds

    bool empty() const { return access_token.empty(); }
    bool expired(int64_t now_ms) const { return expiry_date_ms != 0 && now_ms >= expiry_date_ms; }
    bool needs_refresh(int64_t now_ms, int64_t margin_ms = 60'000) const {
        return expiry_date_ms != 0 && now_ms + margin_ms >= expiry_date_ms;
    }
};

// Persist credentials to ~/.qwen/oauth_creds.json (mode 0600).
// Path is also readable/writable by the qwen-code CLI.
class QwenCredentialStore {
public:
    QwenCredentialStore();
    explicit QwenCredentialStore(std::filesystem::path path);

    QwenCredentials load() const;
    bool save(const QwenCredentials& creds) const;
    bool clear() const;

    const std::filesystem::path& path() const { return path_; }

private:
    std::filesystem::path path_;
};

class QwenOAuth {
public:
    explicit QwenOAuth(hermes::llm::HttpTransport* transport = nullptr);

    // PKCE pair generation (also exposed for tests).
    struct PkcePair { std::string verifier; std::string challenge_s256; };
    static PkcePair generate_pkce();

    // Step 1: request device authorization. Throws on HTTP error.
    QwenDeviceCode request_device_code(const std::string& code_challenge_s256);

    // Step 2: poll token endpoint. Returns nullopt if user denied / expired
    // before timeout, throws on transport error.  Sleeps `interval` between
    // polls, slows down when server signals slow_down.
    std::optional<QwenCredentials> poll_for_token(
        const std::string& device_code,
        const std::string& code_verifier,
        std::chrono::seconds interval = std::chrono::seconds(5),
        std::chrono::seconds max_wait = std::chrono::seconds(1800));

    // Refresh access token — returns the new credentials (with rotated
    // refresh_token if the server issued one).  Returns nullopt on permanent
    // failure (e.g. refresh_token revoked) — caller should re-run the device
    // flow.
    std::optional<QwenCredentials> refresh(const QwenCredentials& current);

    // End-to-end interactive flow with console prompts.  Persists to
    // QwenCredentialStore on success.
    std::optional<QwenCredentials> interactive_login(QwenCredentialStore& store);

    // Get a guaranteed-fresh access_token, transparently refreshing if needed.
    // Returns nullopt if no credentials are stored at all (caller should run
    // interactive_login).  Throws on refresh failure that isn't recoverable.
    std::optional<QwenCredentials> ensure_valid(QwenCredentialStore& store);

private:
    hermes::llm::HttpTransport* transport_;
};

// Resolve the OpenAI-compatible API base URL from a credentials' resource_url
// (e.g. "portal.qwen.ai" -> "https://portal.qwen.ai/v1") with the default
// DashScope endpoint as a fallback.
std::string qwen_api_base_url(const QwenCredentials& creds);

}  // namespace hermes::auth
