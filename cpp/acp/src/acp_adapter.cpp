#include "hermes/acp/acp_adapter.hpp"

#include <chrono>
#include <cstdlib>
#include <random>
#include <sstream>

namespace hermes::acp {

namespace {

// Probe environment for known API key vars. Returns first non-empty value.
std::string probe_env_token() {
    static const char* kVars[] = {
        "ANTHROPIC_API_KEY",
        "OPENAI_API_KEY",
        "OPENROUTER_API_KEY",
    };
    for (const char* v : kVars) {
        if (const char* p = std::getenv(v); p && *p) {
            return std::string(p);
        }
    }
    return {};
}

bool is_supported_method(const std::string& m) {
    return m == "api-key" || m == "oauth";
}

}  // namespace

AcpAdapter::AcpAdapter(AcpConfig config) : config_(std::move(config)) {
    env_token_ = config_.forced_env_token.has_value()
                     ? *config_.forced_env_token
                     : probe_env_token();
}

void AcpAdapter::start() {
    running_.store(true, std::memory_order_release);
    // Stub: real implementation would start an HTTP server.
}

void AcpAdapter::stop() {
    running_.store(false, std::memory_order_release);
}

bool AcpAdapter::running() const {
    return running_.load(std::memory_order_acquire);
}

nlohmann::json AcpAdapter::capabilities() const {
    return {{"name", "hermes"},
            {"version", "0.1.0"},
            {"protocol", "acp"},
            {"listen_address", config_.listen_address},
            {"listen_port", config_.listen_port},
            {"auth_methods", auth_methods()},
            {"capabilities",
             {{"code_actions", true},
              {"diagnostics", true},
              {"completions", true},
              {"chat", true}}}};
}

nlohmann::json AcpAdapter::auth_methods() const {
    nlohmann::json arr = nlohmann::json::array();
    arr.push_back({
        {"id", "api-key"},
        {"name", "API key"},
        {"description",
         "Authenticate with ANTHROPIC_API_KEY / OPENAI_API_KEY or an explicit api_key param."},
    });
    arr.push_back({
        {"id", "oauth"},
        {"name", "OAuth access token"},
        {"description", "Authenticate with an OAuth access_token param."},
    });
    return arr;
}

std::string AcpAdapter::mint_session_id_locked() const {
    // Monotonic + 64-bit random hex. Not cryptographically secure — good
    // enough for a local stdio adapter. Replace with OS RNG if this ever
    // gets exposed over a real network socket.
    static std::mt19937_64 rng{
        static_cast<uint64_t>(
            std::chrono::steady_clock::now().time_since_epoch().count())};
    std::ostringstream os;
    os << std::hex
       << std::chrono::steady_clock::now().time_since_epoch().count()
       << "-" << rng();
    return os.str();
}

AuthResult AcpAdapter::authenticate(const std::string& method_id,
                                    const nlohmann::json& params) {
    AuthResult r;
    r.method_id = method_id;

    if (!is_supported_method(method_id)) {
        r.error = "unsupported auth method: " + method_id;
        return r;
    }

    std::string supplied_token;
    if (method_id == "api-key") {
        if (params.contains("api_key") && params["api_key"].is_string()) {
            supplied_token = params["api_key"].get<std::string>();
        }
        // Fall through to env_token_ if no explicit key.
        if (supplied_token.empty()) supplied_token = env_token_;
    } else if (method_id == "oauth") {
        if (params.contains("access_token") &&
            params["access_token"].is_string()) {
            supplied_token = params["access_token"].get<std::string>();
        }
    }

    if (supplied_token.empty()) {
        r.error = "missing credential for method: " + method_id;
        return r;
    }

    // Issue a session id bound to this method.
    {
        std::lock_guard<std::mutex> lk(sessions_mu_);
        r.session_id = mint_session_id_locked();
        sessions_[r.session_id] = method_id;
    }
    r.ok = true;
    return r;
}

bool AcpAdapter::is_authenticated(const std::string& session_id) const {
    if (session_id.empty()) {
        // Env-provided token grants implicit access only when no session id is
        // required (e.g. single-user stdio mode).
        return !env_token_.empty();
    }
    std::lock_guard<std::mutex> lk(sessions_mu_);
    return sessions_.find(session_id) != sessions_.end();
}

nlohmann::json AcpAdapter::handle_request(const nlohmann::json& request) {
    const auto method = request.value("method", std::string{});

    if (method == "capabilities" || method == "initialize") {
        return capabilities();
    }

    if (method == "authenticate") {
        nlohmann::json params =
            request.value("params", nlohmann::json::object());
        std::string method_id = request.value("method_id", std::string{});
        if (method_id.empty()) {
            method_id = params.value("method_id", std::string{});
        }
        if (method_id.empty()) {
            method_id = params.value("method", std::string{});
        }
        if (method_id.empty()) method_id = "api-key";
        auto result = authenticate(method_id, params);
        if (!result.ok) {
            return {{"status", "error"},
                    {"error", result.error},
                    {"method_id", result.method_id}};
        }
        return {{"status", "ok"},
                {"session_id", result.session_id},
                {"method_id", result.method_id}};
    }

    // Any other method requires authentication.
    const auto session_id = request.value("session_id", std::string{});
    if (!is_authenticated(session_id)) {
        return {{"status", "error"},
                {"error", "unauthenticated"},
                {"method", method}};
    }

    // Stub: all other methods return not_implemented (but pass auth).
    return {{"status", "not_implemented"}, {"method", method}};
}

}  // namespace hermes::acp
