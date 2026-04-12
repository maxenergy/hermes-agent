// ManagedModalEnvironment — run commands through the Nous tool-gateway
// proxy that fronts Modal.com on behalf of multiple tenants.
//
// The gateway exposes the same sandbox/exec semantics as Modal directly
// but authenticates via a single opaque bearer token.  We keep a
// separate class (rather than a Config flag on ModalEnvironment) because
// the URL shape, error handling and connect-timeout discipline differ.
#pragma once

#include "hermes/environments/base.hpp"
#include "hermes/environments/snapshot_store.hpp"
#include "hermes/llm/llm_client.hpp"

#include <chrono>
#include <memory>
#include <optional>
#include <string>

namespace hermes::environments {

class ManagedModalEnvironment : public BaseEnvironment {
public:
    struct Config {
        std::string gateway_url = "https://tool-gateway.nousresearch.com";
        std::string api_token;
        std::optional<std::string> task_id;
        std::chrono::seconds connect_timeout{1};
        std::chrono::seconds poll_interval{5};
    };

    ManagedModalEnvironment();
    explicit ManagedModalEnvironment(Config config);
    ManagedModalEnvironment(Config config,
                            hermes::llm::HttpTransport* transport,
                            std::shared_ptr<SnapshotStore> store);
    ~ManagedModalEnvironment() override;

    std::string name() const override { return "managed_modal"; }

    CompletedProcess execute(const std::string& cmd,
                             const ExecuteOptions& opts) override;
    void cleanup() override;

    const std::string& sandbox_id() const { return sandbox_id_; }

private:
    bool ensure_sandbox(std::string& error);
    std::unordered_map<std::string, std::string> auth_headers() const;

    Config config_;
    hermes::llm::HttpTransport* transport_;
    std::shared_ptr<SnapshotStore> store_;
    std::string sandbox_id_;
};

}  // namespace hermes::environments
