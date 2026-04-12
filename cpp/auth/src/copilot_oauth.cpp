#include "hermes/auth/copilot_oauth.hpp"

#include <nlohmann/json.hpp>

#include <chrono>
#include <iostream>
#include <stdexcept>
#include <thread>

#include "hermes/llm/llm_client.hpp"

namespace hermes::auth {

using json = nlohmann::json;

CopilotOAuth::CopilotOAuth(hermes::llm::HttpTransport* transport, std::string client_id)
    : transport_(transport ? transport : hermes::llm::get_default_transport()),
      client_id_(std::move(client_id)) {}

DeviceCodeResponse CopilotOAuth::request_device_code() {
    if (!transport_) {
        throw std::runtime_error("CopilotOAuth: no HTTP transport");
    }
    json body = {{"client_id", client_id_}, {"scope", "read:user"}};
    auto resp = transport_->post_json(
        "https://github.com/login/device/code",
        {{"Accept", "application/json"}, {"Content-Type", "application/json"}},
        body.dump());
    if (resp.status_code < 200 || resp.status_code >= 300) {
        throw std::runtime_error("GitHub device_code request failed: HTTP " +
                                 std::to_string(resp.status_code));
    }
    auto j = json::parse(resp.body);
    DeviceCodeResponse out;
    out.device_code = j.value("device_code", "");
    out.user_code = j.value("user_code", "");
    out.verification_uri = j.value("verification_uri", "https://github.com/login/device");
    out.expires_in = j.value("expires_in", 900);
    out.interval = j.value("interval", 5);
    return out;
}

std::optional<TokenResponse> CopilotOAuth::poll_for_token(
    const std::string& device_code,
    std::chrono::seconds interval,
    std::chrono::seconds max_wait) {
    if (!transport_) return std::nullopt;

    auto deadline = std::chrono::steady_clock::now() + max_wait;
    auto sleep_for = interval;

    while (std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(sleep_for);
        json body = {
            {"client_id", client_id_},
            {"device_code", device_code},
            {"grant_type", "urn:ietf:params:oauth:grant-type:device_code"}};
        auto resp = transport_->post_json(
            "https://github.com/login/oauth/access_token",
            {{"Accept", "application/json"}, {"Content-Type", "application/json"}},
            body.dump());
        if (resp.status_code < 200 || resp.status_code >= 300) continue;

        auto j = json::parse(resp.body, nullptr, false);
        if (j.is_discarded()) continue;

        if (j.contains("access_token")) {
            TokenResponse t;
            t.access_token = j.value("access_token", "");
            t.token_type = j.value("token_type", "bearer");
            t.scope = j.value("scope", "");
            return t;
        }

        auto err = j.value("error", "");
        if (err == "authorization_pending") continue;
        if (err == "slow_down") {
            sleep_for += std::chrono::seconds(5);
            continue;
        }
        // expired_token, access_denied, etc.
        return std::nullopt;
    }
    return std::nullopt;
}

std::optional<std::string> CopilotOAuth::get_copilot_token(const std::string& gh_token) {
    if (!transport_) return std::nullopt;
    auto resp = transport_->get(
        "https://api.github.com/copilot_internal/v2/token",
        {{"Authorization", "token " + gh_token},
         {"Accept", "application/json"},
         {"User-Agent", "hermes-cpp"}});
    if (resp.status_code < 200 || resp.status_code >= 300) return std::nullopt;
    auto j = json::parse(resp.body, nullptr, false);
    if (j.is_discarded() || !j.contains("token")) return std::nullopt;
    return j["token"].get<std::string>();
}

std::optional<std::string> CopilotOAuth::interactive_login() {
    try {
        auto dc = request_device_code();
        std::cout << "\nTo authenticate GitHub Copilot:\n"
                  << "  1. Open " << dc.verification_uri << "\n"
                  << "  2. Enter the code: " << dc.user_code << "\n"
                  << "\nWaiting for authorization (up to "
                  << (dc.expires_in / 60) << " minutes)...\n";
        auto tok = poll_for_token(
            dc.device_code,
            std::chrono::seconds(dc.interval),
            std::chrono::seconds(dc.expires_in));
        if (!tok) {
            std::cout << "Authorization failed or timed out.\n";
            return std::nullopt;
        }
        std::cout << "Authorized.\n";
        return tok->access_token;
    } catch (const std::exception& e) {
        std::cerr << "OAuth error: " << e.what() << "\n";
        return std::nullopt;
    }
}

}  // namespace hermes::auth
