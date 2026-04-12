#include "hermes/environments/managed_modal.hpp"

#include "hermes/environments/env_filter.hpp"

#include <chrono>
#include <nlohmann/json.hpp>

namespace hermes::environments {

namespace {
bool is_success(int s) { return s >= 200 && s < 300; }
}  // namespace

ManagedModalEnvironment::ManagedModalEnvironment()
    : config_(), transport_(hermes::llm::get_default_transport()) {}

ManagedModalEnvironment::ManagedModalEnvironment(Config config)
    : config_(std::move(config)),
      transport_(hermes::llm::get_default_transport()) {}

ManagedModalEnvironment::ManagedModalEnvironment(
    Config config,
    hermes::llm::HttpTransport* transport,
    std::shared_ptr<SnapshotStore> store)
    : config_(std::move(config)),
      transport_(transport),
      store_(std::move(store)) {}

ManagedModalEnvironment::~ManagedModalEnvironment() = default;

std::unordered_map<std::string, std::string>
ManagedModalEnvironment::auth_headers() const {
    return {
        {"Content-Type", "application/json"},
        {"Authorization", "Bearer " + config_.api_token},
    };
}

bool ManagedModalEnvironment::ensure_sandbox(std::string& error) {
    if (!sandbox_id_.empty()) return true;

    if (store_ && config_.task_id) {
        auto snap = store_->load(*config_.task_id);
        if (snap && snap->contains("sandbox_id") &&
            (*snap)["sandbox_id"].is_string()) {
            sandbox_id_ = (*snap)["sandbox_id"].get<std::string>();
            return true;
        }
    }

    if (!transport_) {
        error = "ManagedModalEnvironment: no HTTP transport";
        return false;
    }

    nlohmann::json body = nlohmann::json::object();
    if (config_.task_id) body["task_id"] = *config_.task_id;

    try {
        auto resp = transport_->post_json(
            config_.gateway_url + "/modal/sandbox/create",
            auth_headers(),
            body.dump());
        if (!is_success(resp.status_code)) {
            error = "ManagedModalEnvironment: create failed (status " +
                    std::to_string(resp.status_code) + "): " + resp.body;
            return false;
        }
        auto parsed = nlohmann::json::parse(resp.body);
        if (!parsed.contains("sandbox_id") ||
            !parsed["sandbox_id"].is_string()) {
            error = "ManagedModalEnvironment: response missing sandbox_id";
            return false;
        }
        sandbox_id_ = parsed["sandbox_id"].get<std::string>();
        if (store_ && config_.task_id) {
            nlohmann::json snap = {{"sandbox_id", sandbox_id_},
                                   {"provider", "managed_modal"}};
            store_->save(*config_.task_id, snap);
        }
        return true;
    } catch (const std::exception& e) {
        error = std::string("ManagedModalEnvironment: ") + e.what();
        return false;
    }
}

CompletedProcess ManagedModalEnvironment::execute(const std::string& cmd,
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

    auto filtered = filter_env(opts.env_vars);
    nlohmann::json env_obj = nlohmann::json::object();
    for (const auto& [k, v] : filtered) env_obj[k] = v;

    nlohmann::json body = {
        {"sandbox_id", sandbox_id_},
        {"command", cmd},
        {"env", env_obj},
        {"timeout_seconds", static_cast<long>(opts.timeout.count())},
    };
    if (!opts.cwd.empty()) body["cwd"] = opts.cwd.string();

    try {
        auto resp = transport_->post_json(
            config_.gateway_url + "/modal/sandbox/exec",
            auth_headers(),
            body.dump());
        if (!is_success(resp.status_code)) {
            result.exit_code = 1;
            result.stderr_text = "ManagedModal exec failed (status " +
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
        result.stderr_text = std::string("ManagedModalEnvironment: ") + e.what();
    }

    result.duration = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t_start);
    return result;
}

void ManagedModalEnvironment::cleanup() {
    if (sandbox_id_.empty() || !transport_) return;
    try {
        nlohmann::json body = {{"sandbox_id", sandbox_id_}};
        transport_->post_json(
            config_.gateway_url + "/modal/sandbox/terminate",
            auth_headers(),
            body.dump());
    } catch (const std::exception&) {
        // Best-effort.
    }
    if (store_ && config_.task_id) store_->remove(*config_.task_id);
    sandbox_id_.clear();
}

}  // namespace hermes::environments
