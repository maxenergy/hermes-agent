// Graceful restart — flag-based restart request and drain timeout.
#pragma once

#include <chrono>

namespace hermes::gateway {

void request_restart();
bool restart_requested();
void clear_restart_request();

constexpr auto DRAIN_TIMEOUT = std::chrono::seconds(30);

}  // namespace hermes::gateway
