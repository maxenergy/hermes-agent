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
    // ACP runs over stdio JSON-RPC — no socket is opened here.  The
    // process's stdin/stdout pipe is owned by the editor host; the host
    // feeds requests through handle_request().  Flipping the flag lets
    // other components (status probes, tests) see that the adapter is
    // ready to accept RPCs.
    running_.store(true, std::memory_order_release);
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

    nlohmann::json params =
        request.value("params", nlohmann::json::object());

    if (method == "new_session" || method == "session/new") {
        std::lock_guard<std::mutex> lk(sessions_mu_);
        auto new_id = mint_session_id_locked();
        sessions_[new_id] = sessions_.count(session_id)
                                ? sessions_[session_id]
                                : std::string("api-key");
        return {{"status", "ok"}, {"session_id", new_id}};
    }

    if (method == "session/cancel" || method == "cancel") {
        std::string prompt_id = params.value("prompt_id", std::string{});
        if (prompt_id.empty()) prompt_id = session_id;
        cancel_prompt(prompt_id);
        return {{"status", "ok"}, {"cancelled", prompt_id}};
    }

    if (method == "prompt" || method == "session/prompt") {
        PromptHandler handler;
        {
            std::lock_guard<std::mutex> lk(prompt_mu_);
            handler = prompt_handler_;
        }
        if (!handler) {
            // Honest JSON-RPC method-not-found (spec code -32601) when
            // no agent is wired to the adapter.
            return {{"status", "error"},
                    {"error", "method_not_available"},
                    {"detail",
                     "Prompt handler not registered — set via "
                     "AcpAdapter::set_prompt_handler()."},
                    {"method", method}};
        }
        try {
            auto result = handler(params);
            if (result.is_object() && result.contains("error")) {
                return {{"status", "error"},
                        {"error", result.value("error", std::string{"handler error"})},
                        {"method", method}};
            }
            return {{"status", "ok"}, {"result", std::move(result)}};
        } catch (const std::exception& ex) {
            return {{"status", "error"},
                    {"error", std::string("prompt handler threw: ") + ex.what()},
                    {"method", method}};
        }
    }

    if (method == "session/close") {
        {
            std::lock_guard<std::mutex> lk(sessions_mu_);
            sessions_.erase(session_id);
        }
        {
            std::lock_guard<std::mutex> lk(perms_mu_);
            permissions_.erase(session_id);
        }
        return {{"status", "ok"}, {"closed", session_id}};
    }

    if (method == "session/get_permissions") {
        std::string target = params.value("session_id", std::string{});
        if (target.empty()) target = session_id;

        bool known = false;
        {
            std::lock_guard<std::mutex> lk(sessions_mu_);
            known = sessions_.find(target) != sessions_.end();
        }
        if (!known) {
            return {{"status", "error"},
                    {"error", "invalid_params"},
                    {"code", -32602},
                    {"detail", "unknown session_id"},
                    {"method", method}};
        }

        PermissionMatrix snapshot = get_permissions(target);
        return {{"status", "ok"},
                {"session_id", target},
                {"matrix", snapshot.to_json()}};
    }

    if (method == "session/set_permissions") {
        std::string target = params.value("session_id", std::string{});
        if (target.empty()) target = session_id;

        bool known = false;
        {
            std::lock_guard<std::mutex> lk(sessions_mu_);
            known = sessions_.find(target) != sessions_.end();
        }
        if (!known) {
            return {{"status", "error"},
                    {"error", "invalid_params"},
                    {"code", -32602},
                    {"detail", "unknown session_id"},
                    {"method", method}};
        }

        if (!params.contains("matrix") || !params.at("matrix").is_object()) {
            return {{"status", "error"},
                    {"error", "invalid_params"},
                    {"code", -32602},
                    {"detail", "missing or malformed 'matrix' object"},
                    {"method", method}};
        }

        PermissionMatrix parsed =
            PermissionMatrix::from_json(params.at("matrix"));
        set_permissions(target, std::move(parsed));
        return {{"status", "ok"}, {"session_id", target}};
    }

    // JSON-RPC method not found (spec code -32601).
    return {{"status", "error"},
            {"error", "method_not_found"},
            {"code", -32601},
            {"method", method}};
}

PermissionMatrix AcpAdapter::get_permissions(
    const std::string& session_id) const {
    std::lock_guard<std::mutex> lk(perms_mu_);
    auto it = permissions_.find(session_id);
    if (it == permissions_.end()) {
        // Default matrix: conservative defaults, no rules.
        return PermissionMatrix{};
    }
    return it->second;
}

void AcpAdapter::set_permissions(const std::string& session_id,
                                 PermissionMatrix matrix) {
    std::lock_guard<std::mutex> lk(perms_mu_);
    // unordered_map does not support heterogeneous insert_or_assign with
    // a non-copyable/movable type via operator[], but PermissionMatrix
    // is move-assignable, so [] works fine.
    auto [it, inserted] =
        permissions_.emplace(session_id, PermissionMatrix{});
    (void)inserted;
    it->second = std::move(matrix);
}

bool AcpAdapter::has_permission_matrix(
    const std::string& session_id) const {
    std::lock_guard<std::mutex> lk(perms_mu_);
    return permissions_.find(session_id) != permissions_.end();
}

PermissionDecision AcpAdapter::check_permission(
    const std::string& session_id, PermissionScope scope,
    const nlohmann::json& context) const {
    // Copy the matrix out under the lock so evaluate() can run without
    // holding perms_mu_ (the matrix has its own internal lock).
    PermissionMatrix snapshot = get_permissions(session_id);
    return snapshot.evaluate(scope, context);
}

void AcpAdapter::set_prompt_handler(PromptHandler handler) {
    std::lock_guard<std::mutex> lk(prompt_mu_);
    prompt_handler_ = std::move(handler);
}

void AcpAdapter::cancel_prompt(const std::string& prompt_id) {
    if (prompt_id.empty()) return;
    std::lock_guard<std::mutex> lk(prompt_mu_);
    cancelled_prompts_.insert(prompt_id);
}

bool AcpAdapter::is_cancelled(const std::string& prompt_id) const {
    if (prompt_id.empty()) return false;
    std::lock_guard<std::mutex> lk(prompt_mu_);
    return cancelled_prompts_.find(prompt_id) != cancelled_prompts_.end();
}

}  // namespace hermes::acp
