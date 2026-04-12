// GitHub Copilot OAuth device code flow.
// https://docs.github.com/en/developers/apps/building-oauth-apps/authorizing-oauth-apps#device-flow
#pragma once

#include <chrono>
#include <optional>
#include <string>

namespace hermes::llm {
class HttpTransport;
}

namespace hermes::auth {

struct DeviceCodeResponse {
    std::string device_code;
    std::string user_code;
    std::string verification_uri;
    int expires_in = 900;
    int interval = 5;
};

struct TokenResponse {
    std::string access_token;
    std::string token_type;
    std::string scope;
};

class CopilotOAuth {
public:
    // client_id defaults to the Neovim Copilot client ID (public, not a secret).
    explicit CopilotOAuth(hermes::llm::HttpTransport* transport = nullptr,
                          std::string client_id = "Iv1.b507a08c87ecfe98");

    DeviceCodeResponse request_device_code();

    // Polls until timeout. Returns nullopt on expired/denied/error.
    std::optional<TokenResponse> poll_for_token(
        const std::string& device_code,
        std::chrono::seconds interval,
        std::chrono::seconds max_wait = std::chrono::seconds(900));

    // Exchange a GitHub access_token for a Copilot internal token.
    std::optional<std::string> get_copilot_token(const std::string& gh_token);

    // Interactive: runs the full flow with console prompts, returns GH token.
    std::optional<std::string> interactive_login();

private:
    hermes::llm::HttpTransport* transport_;
    std::string client_id_;
};

}  // namespace hermes::auth
