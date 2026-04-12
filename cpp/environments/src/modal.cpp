#include "hermes/environments/modal.hpp"

#include "hermes/environments/env_filter.hpp"

#include <chrono>
#include <nlohmann/json.hpp>

namespace hermes::environments {

namespace {

// Accept both 200 OK and 201 Created as success.
bool is_success(int status) { return status >= 200 && status < 300; }

}  // namespace

ModalEnvironment::ModalEnvironment()
    : config_(), transport_(hermes::llm::get_default_transport()) {}

ModalEnvironment::ModalEnvironment(Config config)
    : config_(std::move(config)),
      transport_(hermes::llm::get_default_transport()) {}

ModalEnvironment::ModalEnvironment(Config config,
                                   hermes::llm::HttpTransport* transport,
                                   std::shared_ptr<SnapshotStore> store)
    : config_(std::move(config)),
      transport_(transport),
      store_(std::move(store)) {}

ModalEnvironment::~ModalEnvironment() = default;

std::unordered_map<std::string, std::string>
ModalEnvironment::auth_headers() const {
    // Modal's token_id/secret map to a single Basic-auth-like bearer.  The
    // gateway / test harness is the final authority on encoding; we send
    // both as explicit headers for determinism and backward compat.
    std::unordered_map<std::string, std::string> h;
    h["Content-Type"] = "application/json";
    if (!config_.token_id.empty()) {
        h["Modal-Token-Id"] = config_.token_id;
    }
    if (!config_.token_secret.empty()) {
        h["Modal-Token-Secret"] = config_.token_secret;
        h["Authorization"] = "Bearer " + config_.token_secret;
    }
    return h;
}

bool ModalEnvironment::ensure_sandbox(std::string& error) {
    if (!sandbox_id_.empty()) return true;

    // Try snapshot first.
    if (store_ && config_.task_id) {
        auto snap = store_->load(*config_.task_id);
        if (snap && snap->contains("sandbox_id") &&
            (*snap)["sandbox_id"].is_string()) {
            sandbox_id_ = (*snap)["sandbox_id"].get<std::string>();
            return true;
        }
    }

    if (!transport_) {
        error = "ModalEnvironment: no HTTP transport available";
        return false;
    }

    nlohmann::json body = {
        {"app_name", config_.app_name},
        {"image", config_.image},
        {"cpus", config_.cpus},
        {"memory", config_.memory},
    };
    if (!config_.packages.empty()) {
        body["packages"] = config_.packages;
    }

    try {
        auto resp = transport_->post_json(
            config_.api_url + "/v1/sandboxes",
            auth_headers(),
            body.dump());
        if (!is_success(resp.status_code)) {
            error = "ModalEnvironment: create sandbox failed (status " +
                    std::to_string(resp.status_code) + "): " + resp.body;
            return false;
        }
        auto parsed = nlohmann::json::parse(resp.body);
        if (!parsed.contains("sandbox_id") ||
            !parsed["sandbox_id"].is_string()) {
            error = "ModalEnvironment: response missing sandbox_id";
            return false;
        }
        sandbox_id_ = parsed["sandbox_id"].get<std::string>();
        if (store_ && config_.task_id) {
            nlohmann::json snap = {{"sandbox_id", sandbox_id_},
                                   {"provider", "modal"}};
            store_->save(*config_.task_id, snap);
        }
        return true;
    } catch (const std::exception& e) {
        error = std::string("ModalEnvironment: ") + e.what();
        return false;
    }
}

CompletedProcess ModalEnvironment::execute(const std::string& cmd,
                                           const ExecuteOptions& opts) {
    auto t_start = std::chrono::steady_clock::now();
    CompletedProcess result;

    std::string err;
    if (!ensure_sandbox(err)) {
        result.exit_code = 1;
        result.stderr_text = err;
        result.duration = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - t_start);
        return result;
    }

    // Build exec payload.
    auto filtered = filter_env(opts.env_vars);
    nlohmann::json env_obj = nlohmann::json::object();
    for (const auto& [k, v] : filtered) env_obj[k] = v;

    nlohmann::json body = {
        {"command", cmd},
        {"env", env_obj},
        {"timeout_seconds", static_cast<long>(opts.timeout.count())},
    };
    if (!opts.cwd.empty()) {
        body["cwd"] = opts.cwd.string();
    }

    try {
        auto resp = transport_->post_json(
            config_.api_url + "/v1/sandboxes/" + sandbox_id_ + "/exec",
            auth_headers(),
            body.dump());
        if (!is_success(resp.status_code)) {
            result.exit_code = 1;
            result.stderr_text = "Modal exec failed (status " +
                                 std::to_string(resp.status_code) +
                                 "): " + resp.body;
        } else {
            auto parsed = nlohmann::json::parse(resp.body);
            if (parsed.contains("exit_code") &&
                parsed["exit_code"].is_number_integer()) {
                result.exit_code = parsed["exit_code"].get<int>();
            } else {
                result.exit_code = 0;
            }
            if (parsed.contains("stdout") && parsed["stdout"].is_string()) {
                result.stdout_text = parsed["stdout"].get<std::string>();
            }
            if (parsed.contains("stderr") && parsed["stderr"].is_string()) {
                result.stderr_text = parsed["stderr"].get<std::string>();
            }
        }
    } catch (const std::exception& e) {
        result.exit_code = 1;
        result.stderr_text = std::string("ModalEnvironment: ") + e.what();
    }

    result.duration = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t_start);
    return result;
}

void ModalEnvironment::cleanup() {
    if (sandbox_id_.empty() || !transport_) return;
    try {
        auto url = config_.api_url + "/v1/sandboxes/" + sandbox_id_;
        // Terminate uses POST per Modal's API shape.
        transport_->post_json(url + "/terminate", auth_headers(), "{}");
    } catch (const std::exception&) {
        // Best-effort cleanup; ignore transport errors.
    }
    if (store_ && config_.task_id) {
        store_->remove(*config_.task_id);
    }
    sandbox_id_.clear();
}

}  // namespace hermes::environments
