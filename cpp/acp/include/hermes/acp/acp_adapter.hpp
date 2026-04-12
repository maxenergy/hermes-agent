// AcpAdapter — editor integration adapter for VS Code / Zed / JetBrains.
#pragma once

#include <atomic>
#include <string>

#include <nlohmann/json.hpp>

namespace hermes::acp {

struct AcpConfig {
    std::string listen_address = "127.0.0.1";
    int listen_port = 8765;
};

class AcpAdapter {
public:
    explicit AcpAdapter(AcpConfig config);

    // Start HTTP server for ACP protocol (stub for Phase 16)
    void start();
    void stop();
    bool running() const;

    // ACP capability registration
    nlohmann::json capabilities() const;

    // Handler for ACP requests (stub)
    nlohmann::json handle_request(const nlohmann::json& request);

private:
    AcpConfig config_;
    std::atomic<bool> running_{false};
};

}  // namespace hermes::acp
