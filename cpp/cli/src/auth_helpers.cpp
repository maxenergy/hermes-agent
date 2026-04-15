// Provider-specific auth helpers — port of hermes_cli/auth.py.
#include "hermes/cli/auth_helpers.hpp"

#include "hermes/cli/auth_core.hpp"

#include <openssl/evp.h>
#include <openssl/sha.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <sys/stat.h>
#include <unistd.h>

namespace hermes::cli::auth_helpers {

namespace fs = std::filesystem;

namespace {

std::string strip(std::string s) {
    size_t i = 0;
    while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i;
    s.erase(0, i);
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) {
        s.pop_back();
    }
    return s;
}

std::string to_lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return s;
}

bool starts_with(const std::string& s, const char* p) {
    const auto n = std::strlen(p);
    return s.size() >= n && std::memcmp(s.data(), p, n) == 0;
}

bool is_executable_file(const std::string& path) {
    struct stat st {};
    if (::stat(path.c_str(), &st) != 0) return false;
    if (!S_ISREG(st.st_mode)) return false;
    return ::access(path.c_str(), X_OK) == 0;
}

std::string env_or_empty(const char* name) {
    const char* v = std::getenv(name);
    return v ? std::string(v) : std::string{};
}

fs::path home_dir() {
    auto e = env_or_empty("HOME");
    if (!e.empty()) return fs::path(e);
    return fs::path("/");
}

std::string sha256_hex(const std::string& input) {
    unsigned char hash[SHA256_DIGEST_LENGTH];
    SHA256(reinterpret_cast<const unsigned char*>(input.data()),
           input.size(), hash);
    std::ostringstream oss;
    for (unsigned char b : hash) {
        oss << std::hex << std::setfill('0') << std::setw(2)
            << static_cast<int>(b);
    }
    return oss.str();
}

// Base64-URL decode (with optional padding accepted).  Returns the
// decoded byte string; on any failure the caller's path is to swallow
// the error and yield an empty payload.
bool base64url_decode(const std::string& input, std::string& out) {
    // Convert URL-safe alphabet to standard, then pad.
    std::string s = input;
    for (auto& c : s) {
        if (c == '-') c = '+';
        else if (c == '_') c = '/';
    }
    while (s.size() % 4) s.push_back('=');

    out.clear();
    out.resize(s.size());
    int len = EVP_DecodeBlock(reinterpret_cast<unsigned char*>(out.data()),
                              reinterpret_cast<const unsigned char*>(s.data()),
                              static_cast<int>(s.size()));
    if (len < 0) return false;
    // Trim padding bytes.
    int pad = 0;
    for (auto it = s.rbegin(); it != s.rend() && *it == '='; ++it) ++pad;
    out.resize(static_cast<size_t>(std::max(0, len - pad)));
    return true;
}

}  // namespace

// -------------------------------------------------------------------------
// Placeholder secret denylist.
// -------------------------------------------------------------------------

const std::unordered_set<std::string>& placeholder_secret_values() {
    static const std::unordered_set<std::string> v = {
        "*", "**", "***", "changeme", "your_api_key", "your-api-key",
        "placeholder", "example", "dummy", "null", "none",
    };
    return v;
}

bool has_usable_secret(const std::string& value, std::size_t min_length) {
    auto cleaned = strip(value);
    if (cleaned.size() < min_length) return false;
    if (placeholder_secret_values().count(to_lower(cleaned))) return false;
    return true;
}

// -------------------------------------------------------------------------
// Provider URL routing.
// -------------------------------------------------------------------------

std::string resolve_kimi_base_url(const std::string& api_key,
                                  const std::string& default_url,
                                  const std::string& env_override) {
    if (!env_override.empty()) return env_override;
    if (starts_with(api_key, "sk-kimi-")) return kKimiCodeBaseUrl;
    return default_url;
}

const std::vector<ZaiEndpoint>& zai_endpoints() {
    static const std::vector<ZaiEndpoint> v = {
        {"global", "https://api.z.ai/api/paas/v4", "glm-5", "Global"},
        {"cn", "https://open.bigmodel.cn/api/paas/v4", "glm-5", "China"},
        {"coding-global", "https://api.z.ai/api/coding/paas/v4", "glm-4.7",
         "Global (Coding Plan)"},
        {"coding-cn", "https://open.bigmodel.cn/api/coding/paas/v4", "glm-4.7",
         "China (Coding Plan)"},
    };
    return v;
}

std::string zai_key_hash(const std::string& api_key) {
    return sha256_hex(api_key).substr(0, 16);
}

// -------------------------------------------------------------------------
// GitHub CLI discovery.
// -------------------------------------------------------------------------

std::vector<std::string> gh_cli_candidates() {
    std::vector<std::string> out;
    auto path_env = env_or_empty("PATH");
    if (!path_env.empty()) {
        std::string cur;
        std::vector<std::string> dirs;
        for (char c : path_env) {
            if (c == ':') {
                dirs.push_back(cur);
                cur.clear();
            } else {
                cur.push_back(c);
            }
        }
        dirs.push_back(cur);
        for (const auto& d : dirs) {
            if (d.empty()) continue;
            auto candidate = d + "/gh";
            if (is_executable_file(candidate)) {
                out.push_back(candidate);
                break;  // first PATH match wins (matches `shutil.which`).
            }
        }
    }

    const std::array<std::string, 3> extras = {
        "/opt/homebrew/bin/gh", "/usr/local/bin/gh",
        (home_dir() / ".local" / "bin" / "gh").string(),
    };
    for (const auto& candidate : extras) {
        if (std::find(out.begin(), out.end(), candidate) != out.end()) continue;
        if (is_executable_file(candidate)) out.push_back(candidate);
    }
    return out;
}

// -------------------------------------------------------------------------
// JWT.
// -------------------------------------------------------------------------

nlohmann::json decode_jwt_claims(const std::string& token) {
    if (std::count(token.begin(), token.end(), '.') != 2) {
        return nlohmann::json::object();
    }
    auto first_dot = token.find('.');
    auto second_dot = token.find('.', first_dot + 1);
    auto payload = token.substr(first_dot + 1, second_dot - first_dot - 1);

    std::string raw;
    if (!base64url_decode(payload, raw)) return nlohmann::json::object();
    try {
        auto j = nlohmann::json::parse(raw);
        if (!j.is_object()) return nlohmann::json::object();
        return j;
    } catch (...) {
        return nlohmann::json::object();
    }
}

bool codex_access_token_is_expiring(const std::string& access_token,
                                    int skew_seconds) {
    auto claims = decode_jwt_claims(access_token);
    auto it = claims.find("exp");
    if (it == claims.end()) return false;
    if (!it->is_number()) return false;
    const double exp = it->get<double>();
    const auto now = std::chrono::duration<double>(
                         std::chrono::system_clock::now().time_since_epoch())
                         .count();
    const int skew = std::max(0, skew_seconds);
    return exp <= (now + static_cast<double>(skew));
}

bool qwen_access_token_is_expiring(const nlohmann::json& expiry_date_ms,
                                   int skew_seconds) {
    long long expiry_ms = 0;
    if (expiry_date_ms.is_number_integer()) {
        expiry_ms = expiry_date_ms.get<long long>();
    } else if (expiry_date_ms.is_number_float()) {
        expiry_ms = static_cast<long long>(expiry_date_ms.get<double>());
    } else if (expiry_date_ms.is_string()) {
        try {
            expiry_ms = std::stoll(expiry_date_ms.get<std::string>());
        } catch (...) {
            return true;
        }
    } else {
        return true;
    }
    const auto now = std::chrono::duration<double>(
                         std::chrono::system_clock::now().time_since_epoch())
                         .count();
    const int skew = std::max(0, skew_seconds);
    return (now + static_cast<double>(skew)) * 1000.0 >=
           static_cast<double>(expiry_ms);
}

// -------------------------------------------------------------------------
// Qwen CLI credential file.
// -------------------------------------------------------------------------

fs::path qwen_cli_auth_path() {
    return home_dir() / ".qwen" / "oauth_creds.json";
}

nlohmann::json read_qwen_cli_tokens() {
    auto path = qwen_cli_auth_path();
    if (!fs::exists(path)) {
        throw auth_core::AuthError(
            "Qwen CLI credentials not found. Run 'qwen auth qwen-oauth' first.",
            "qwen-oauth", std::string("qwen_auth_missing"));
    }
    std::ifstream ifs(path);
    if (!ifs) {
        throw auth_core::AuthError(
            std::string("Failed to read Qwen CLI credentials from ") +
                path.string(),
            "qwen-oauth", std::string("qwen_auth_read_failed"));
    }
    std::stringstream ss;
    ss << ifs.rdbuf();
    nlohmann::json data;
    try {
        data = nlohmann::json::parse(ss.str());
    } catch (const std::exception& exc) {
        throw auth_core::AuthError(
            std::string("Failed to read Qwen CLI credentials from ") +
                path.string() + ": " + exc.what(),
            "qwen-oauth", std::string("qwen_auth_read_failed"));
    }
    if (!data.is_object()) {
        throw auth_core::AuthError(
            std::string("Invalid Qwen CLI credentials in ") + path.string() + ".",
            "qwen-oauth", std::string("qwen_auth_invalid"));
    }
    return data;
}

fs::path save_qwen_cli_tokens(const nlohmann::json& tokens) {
    auto path = qwen_cli_auth_path();
    fs::create_directories(path.parent_path());
    auto tmp = path;
    tmp.replace_extension(".tmp");
    {
        std::ofstream ofs(tmp, std::ios::trunc);
        ofs << tokens.dump(2) << "\n";
    }
    ::chmod(tmp.c_str(), S_IRUSR | S_IWUSR);
    fs::rename(tmp, path);
    return path;
}

}  // namespace hermes::cli::auth_helpers
