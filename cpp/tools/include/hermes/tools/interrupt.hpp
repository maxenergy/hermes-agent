// Process-global interrupt signal — tools poll this to bail out of
// long-running operations when the user presses Ctrl-C.
#pragma once

#include <atomic>

namespace hermes::tools {

class InterruptFlag {
public:
    static InterruptFlag& global();

    void request() noexcept { flag_.store(true, std::memory_order_release); }
    void clear() noexcept { flag_.store(false, std::memory_order_release); }
    bool requested() const noexcept {
        return flag_.load(std::memory_order_acquire);
    }

private:
    InterruptFlag() = default;
    std::atomic<bool> flag_{false};
};

}  // namespace hermes::tools
