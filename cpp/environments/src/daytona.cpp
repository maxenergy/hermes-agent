#include "hermes/environments/daytona.hpp"

#include "hermes/environments/env_filter.hpp"

#include <chrono>
#include <nlohmann/json.hpp>

namespace hermes::environments {

namespace {
bool is_success(int s) { return s >= 200 && s < 300; }
}  // namespace

DaytonaEnvironment::DaytonaEnvironment()
    : config_(), transport_(hermes::llm::get_default_transport()) {}

DaytonaEnvironment::DaytonaEnvironment(Config config)
    : config_(std::move(config)),
      transport_(hermes::llm::get_default_transport()) {}

DaytonaEnvironment::DaytonaEnvironment(Config config,
                                       hermes::llm::HttpTransport* transport,
                                       std::shared_ptr<SnapshotStore> store)
    : config_(std::move(config)),
      transport_(transport),
      store_(std::move(store)) {}

DaytonaEnvironment::~DaytonaEnvironment() = default;

std::unordered_map<std::string, std::string>
DaytonaEnvironment::auth_headers() const {
    return {
        {"Content-Type", "application/json"},
        {"Authorization", "Bearer " + config_.api_token},
    };
}

bool DaytonaEnvironment::ensure_workspace(std::string& error) {
    if (!workspace_id_.empty()) return true;

    if (store_ && config_.task_id) {
        auto snap = store_->load(*config_.task_id);
        if (snap && snap->contains("workspace_id") &&
            (*snap)["workspace_id"].is_string()) {
            workspace_id_ = (*snap)["workspace_id"].get<std::string>();
            return true;
        }
    }

    if (!transport_) {
        error = "DaytonaEnvironment: no HTTP transport available";
        return false;
    }

    nlohmann::json body = {
        {"image", config_.image},
        {"cpu", config_.cpus},
        {"memory", config_.memory_gib},
        {"disk", config_.disk_gib},
    };

    try {
        auto resp = transport_->post_json(
            config_.api_url + "/workspace",
            auth_headers(),
            body.dump());
        if (!is_success(resp.status_code)) {
            error = "DaytonaEnvironment: create workspace failed (status " +
                    std::to_string(resp.status_code) + "): " + resp.body;
            return false;
        }
        auto parsed = nlohmann::json::parse(resp.body);
        std::string id;
        if (parsed.contains("id") && parsed["id"].is_string()) {
            id = parsed["id"].get<std::string>();
        } else if (parsed.contains("workspace_id") &&
                   parsed["workspace_id"].is_string()) {
            id = parsed["workspace_id"].get<std::string>();
        } else {
            error = "DaytonaEnvironment: response missing workspace id";
            return false;
        }
        workspace_id_ = id;
        if (store_ && config_.task_id) {
            nlohmann::json snap = {{"workspace_id", workspace_id_},
                                   {"provider", "daytona"}};
            store_->save(*config_.task_id, snap);
        }
        return true;
    } catch (const std::exception& e) {
        error = std::string("DaytonaEnvironment: ") + e.what();
        return false;
    }
}

CompletedProcess DaytonaEnvironment::execute(const std::string& cmd,
                                             const ExecuteOptions& opts) {
    auto t_start = std::chrono::steady_clock::now();
    CompletedProcess result;

    std::string err;
    if (!ensure_workspace(err)) {
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
        {"command", cmd},
        {"env", env_obj},
        {"timeout", static_cast<long>(opts.timeout.count())},
    };
    if (!opts.cwd.empty()) body["cwd"] = opts.cwd.string();

    try {
        auto resp = transport_->post_json(
            config_.api_url + "/workspace/" + workspace_id_ + "/exec",
            auth_headers(),
            body.dump());
        if (!is_success(resp.status_code)) {
            result.exit_code = 1;
            result.stderr_text = "Daytona exec failed (status " +
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
        result.stderr_text = std::string("DaytonaEnvironment: ") + e.what();
    }

    result.duration = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t_start);
    return result;
}

void DaytonaEnvironment::cleanup() {
    if (workspace_id_.empty() || !transport_) return;
    try {
        transport_->post_json(
            config_.api_url + "/workspace/" + workspace_id_ + "/delete",
            auth_headers(),
            "{}");
    } catch (const std::exception&) {
        // Best-effort cleanup.
    }
    if (store_ && config_.task_id) store_->remove(*config_.task_id);
    workspace_id_.clear();
}

}  // namespace hermes::environments
