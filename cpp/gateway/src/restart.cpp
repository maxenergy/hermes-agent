#include <hermes/gateway/restart.hpp>

#include <atomic>

namespace hermes::gateway {

namespace {
std::atomic<bool> g_restart_requested{false};
}

void request_restart() {
    g_restart_requested.store(true, std::memory_order_release);
}

bool restart_requested() {
    return g_restart_requested.load(std::memory_order_acquire);
}

void clear_restart_request() {
    g_restart_requested.store(false, std::memory_order_release);
}

}  // namespace hermes::gateway
