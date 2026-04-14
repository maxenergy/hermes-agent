// C++17 port of agent/retry_utils.py.
#include "hermes/agent/retry_utils.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <random>

namespace hermes::agent {

namespace {

std::atomic<std::size_t> g_jitter_counter{0};

}  // namespace

double jittered_backoff(int attempt,
                        double base_delay,
                        double max_delay,
                        double jitter_ratio) {
    const std::size_t tick = g_jitter_counter.fetch_add(1, std::memory_order_relaxed) + 1;

    const int exponent = std::max(0, attempt - 1);
    double delay;
    if (exponent >= 63 || base_delay <= 0.0) {
        delay = max_delay;
    } else {
        // Compute 2^exponent via bit shift for integer exponents <= 62.
        const double factor = static_cast<double>(static_cast<std::uint64_t>(1) << exponent);
        delay = std::min(base_delay * factor, max_delay);
    }

    const auto now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                            std::chrono::system_clock::now().time_since_epoch())
                            .count();
    const std::uint64_t seed = static_cast<std::uint64_t>(now_ns) ^
                               (static_cast<std::uint64_t>(tick) * 0x9E3779B9ULL);
    std::mt19937 rng(static_cast<std::mt19937::result_type>(seed & 0xFFFFFFFFULL));
    std::uniform_real_distribution<double> dist(0.0, jitter_ratio * delay);
    const double jitter = dist(rng);

    return delay + jitter;
}

std::size_t jitter_tick_count_for_testing() {
    return g_jitter_counter.load(std::memory_order_relaxed);
}

}  // namespace hermes::agent
