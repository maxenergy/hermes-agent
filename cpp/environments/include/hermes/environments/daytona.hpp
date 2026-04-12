// DaytonaEnvironment — run commands inside a Daytona workspace/sandbox.
//
// Workflow:
//   POST  {api_url}/workspace                → create, returns workspace id
//   POST  {api_url}/workspace/{id}/exec      → run a command
//   DELETE {api_url}/workspace/{id}          → cleanup (via POST fallback)
//
// Uses `hermes::llm::get_default_transport()` for HTTP unless a custom
// transport is injected at construction time (test hook).
#pragma once

#include "hermes/environments/base.hpp"
#include "hermes/environments/snapshot_store.hpp"
#include "hermes/llm/llm_client.hpp"

#include <memory>
#include <optional>
#include <string>

namespace hermes::environments {

class DaytonaEnvironment : public BaseEnvironment {
public:
    struct Config {
        std::string api_url = "https://app.daytona.io/api";
        std::string api_token;
        std::string image = "ubuntu:24.04";
        std::optional<std::string> task_id;
        int cpus = 1;
        int memory_gib = 2;
        int disk_gib = 10;
    };

    DaytonaEnvironment();
    explicit DaytonaEnvironment(Config config);
    DaytonaEnvironment(Config config,
                       hermes::llm::HttpTransport* transport,
                       std::shared_ptr<SnapshotStore> store);
    ~DaytonaEnvironment() override;

    std::string name() const override { return "daytona"; }

    CompletedProcess execute(const std::string& cmd,
                             const ExecuteOptions& opts) override;
    void cleanup() override;

    const std::string& workspace_id() const { return workspace_id_; }

private:
    bool ensure_workspace(std::string& error);
    std::unordered_map<std::string, std::string> auth_headers() const;

    Config config_;
    hermes::llm::HttpTransport* transport_;  // non-owning
    std::shared_ptr<SnapshotStore> store_;
    std::string workspace_id_;
};

}  // namespace hermes::environments
