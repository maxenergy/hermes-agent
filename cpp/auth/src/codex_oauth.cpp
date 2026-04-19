#include "hermes/auth/codex_oauth.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <sstream>
#include <vector>

namespace hermes::auth {

namespace {

using json = nlohmann::json;

std::string get_env(const char* name) {
    const char* v = std::getenv(name);
    return v ? std::string(v) : std::string();
}

std::chrono::system_clock::time_point parse_iso8601(const std::string& s) {
    // Minimal ISO-8601 parser: "YYYY-MM-DDTHH:MM:SS(.fff)?Z".  Returns
    // epoch on parse failure.
    if (s.empty()) return {};
    std::tm tm{};
    int y = 0, mo = 0, d = 0, h = 0, mi = 0, sec = 0;
    char t = 0, z = 0;
    std::istringstream iss(s);
    iss >> y; iss.get();             // '-'
    iss >> mo; iss.get();            // '-'
    iss >> d; iss >> t;              // 'T'
    iss >> h; iss.get();             // ':'
    iss >> mi; iss.get();            // ':'
    iss >> sec;
    // Optional fractional seconds and trailing 'Z'
    if (iss.peek() == '.') {
        iss.get();
        while (std::isdigit(iss.peek())) iss.get();
    }
    if (iss.peek() == 'Z') iss >> z;
    if (y < 1970) return {};
    tm.tm_year = y - 1900;
    tm.tm_mon = mo - 1;
    tm.tm_mday = d;
    tm.tm_hour = h;
    tm.tm_min = mi;
    tm.tm_sec = sec;
#if defined(_WIN32)
    std::time_t tt = _mkgmtime(&tm);
#else
    std::time_t tt = ::timegm(&tm);
#endif
    if (tt < 0) return {};
    return std::chrono::system_clock::from_time_t(tt);
}

}  // namespace

std::filesystem::path codex_home() {
    auto override_dir = get_env("CODEX_HOME");
    if (!override_dir.empty()) {
        return std::filesystem::path(override_dir);
    }
    auto home = get_env("HOME");
    if (home.empty()) {
        // Best-effort fallback.
        return std::filesystem::path(".codex");
    }
    return std::filesystem::path(home) / ".codex";
}

std::optional<CodexCredentials> load_codex_credentials_from(
    const std::filesystem::path& auth_json_path) {
    std::ifstream ifs(auth_json_path);
    if (!ifs.is_open()) return std::nullopt;
    std::ostringstream ss;
    ss << ifs.rdbuf();
    auto raw = ss.str();
    if (raw.empty()) return std::nullopt;

    auto parsed = json::parse(raw, nullptr, /*allow_exceptions=*/false);
    if (parsed.is_discarded() || !parsed.is_object()) return std::nullopt;

    CodexCredentials out;
    out.auth_mode = parsed.value("auth_mode", std::string{});
    // The Codex CLI stores OPENAI_API_KEY at the top level when
    // auth_mode=="apikey"; null when unset.
    if (parsed.contains("OPENAI_API_KEY") && parsed["OPENAI_API_KEY"].is_string()) {
        out.api_key = parsed["OPENAI_API_KEY"].get<std::string>();
    }
    if (parsed.contains("tokens") && parsed["tokens"].is_object()) {
        const auto& t = parsed["tokens"];
        out.access_token = t.value("access_token", std::string{});
        out.id_token = t.value("id_token", std::string{});
        out.refresh_token = t.value("refresh_token", std::string{});
        out.account_id = t.value("account_id", std::string{});
    }
    out.last_refresh = parse_iso8601(parsed.value("last_refresh", std::string{}));

    // Reject if we got nothing useful at all.
    if (out.auth_mode.empty() && out.access_token.empty() &&
        out.api_key.empty()) {
        return std::nullopt;
    }
    return out;
}

std::optional<CodexCredentials> load_codex_credentials() {
    return load_codex_credentials_from(codex_home() / "auth.json");
}

// Ports upstream Python commit 2a2e5c0f: refresh-endpoint 401/403 always
// invalidates the refresh_token, regardless of whether the JSON body
// happens to carry one of the known error codes.
CodexRefreshClassification classify_codex_refresh_response(
    int status_code,
    const std::string& response_body) {
    CodexRefreshClassification out;
    auto parsed = json::parse(response_body, nullptr, /*allow_exceptions=*/false);
    if (!parsed.is_discarded() && parsed.is_object()) {
        if (parsed.contains("error") && parsed["error"].is_string()) {
            out.error_code = parsed["error"].get<std::string>();
        }
        if (parsed.contains("error_description") &&
            parsed["error_description"].is_string()) {
            out.message = parsed["error_description"].get<std::string>();
        }
    }

    static const std::vector<std::string> kReloginCodes = {
        "invalid_grant", "invalid_token", "access_denied",
        "unauthorized_client", "expired_token"};
    bool body_forces_relogin =
        !out.error_code.empty() &&
        std::find(kReloginCodes.begin(), kReloginCodes.end(), out.error_code) !=
            kReloginCodes.end();

    if (body_forces_relogin) {
        out.relogin_required = true;
    }
    // 401/403 from a token endpoint always mean the refresh token is
    // invalid — force relogin even if the body code wasn't recognised.
    if (status_code == 401 || status_code == 403) {
        out.relogin_required = true;
    }
    if (status_code >= 500) {
        out.transient = true;
        out.relogin_required = false;
    }

    if (out.message.empty()) {
        if (out.relogin_required) {
            out.message = "Codex refresh token is no longer valid. "
                          "Run `hermes auth openai-codex` to re-authenticate.";
        } else if (out.transient) {
            out.message = "Codex token endpoint returned HTTP " +
                          std::to_string(status_code) + " (transient).";
        } else if (status_code < 200 || status_code >= 300) {
            out.message = "Codex token endpoint returned HTTP " +
                          std::to_string(status_code) + ".";
        }
    }
    return out;
}

}  // namespace hermes::auth
