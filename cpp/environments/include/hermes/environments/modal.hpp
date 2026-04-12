// ModalEnvironment — run commands inside a Modal.com sandbox.
//
// On first `execute()` the environment either rehydrates a sandbox id
// from the optional SnapshotStore (keyed on `task_id`) or creates a new
// one via POST /v1/sandboxes.  Subsequent commands are sent to
// POST /v1/sandboxes/{id}/exec.  HTTP traffic goes through the process
// default transport so tests can swap in a FakeHttpTransport.
#pragma once

#include "hermes/environments/base.hpp"
#include "hermes/environments/snapshot_store.hpp"

#include "hermes/llm/llm_client.hpp"

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace hermes::environments {

class ModalEnvironment : public BaseEnvironment {
public:
    struct Config {
        std::string token_id;
        std::string token_secret;
        std::string app_name = "hermes-agent";
        std::string image = "python:3.11-slim";
        std::vector<std::string> packages;
        std::optional<std::string> task_id;
        double cpus = 0.5;
        std::string memory = "1Gi";
        std::string api_url = "https://api.modal.com";
    };

    ModalEnvironment();
    explicit ModalEnvironment(Config config);
    // Testing: override transport and/or snapshot store.
    ModalEnvironment(Config config,
                     hermes::llm::HttpTransport* transport,
                     std::shared_ptr<SnapshotStore> store);
    ~ModalEnvironment() override;

    std::string name() const override { return "modal"; }

    CompletedProcess execute(const std::string& cmd,
                             const ExecuteOptions& opts) override;
    void cleanup() override;

    // Exposed for testing.
    const std::string& sandbox_id() const { return sandbox_id_; }

private:
    // Returns an existing sandbox id (from snapshot) or creates one.
    // Populates `error` and returns false on any HTTP failure.
    bool ensure_sandbox(std::string& error);

    std::unordered_map<std::string, std::string> auth_headers() const;

    Config config_;
    hermes::llm::HttpTransport* transport_;  // non-owning
    std::shared_ptr<SnapshotStore> store_;
    std::string sandbox_id_;
};

}  // namespace hermes::environments
