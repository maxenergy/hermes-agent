#include "hermes/acp/acp_adapter.hpp"

namespace hermes::acp {

AcpAdapter::AcpAdapter(AcpConfig config) : config_(std::move(config)) {}

void AcpAdapter::start() {
    running_.store(true, std::memory_order_release);
    // Stub: real implementation would start an HTTP server
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
            {"capabilities",
             {{"code_actions", true},
              {"diagnostics", true},
              {"completions", true},
              {"chat", true}}}};
}

nlohmann::json AcpAdapter::handle_request(const nlohmann::json& request) {
    auto method = request.value("method", "");

    if (method == "capabilities") {
        return capabilities();
    }

    // Stub: all other methods return not_implemented
    return {{"status", "not_implemented"}, {"method", method}};
}

}  // namespace hermes::acp
