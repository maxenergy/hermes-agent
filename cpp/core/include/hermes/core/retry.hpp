// Jittered exponential-backoff helper shared by all retry loops.
#pragma once

#include <chrono>

namespace hermes::core::retry {

// Return the delay for the given retry `attempt` (1-based). The base
// delay doubles per attempt up to `max_delay`, plus a uniform jitter
// of `base * jitter_ratio * 2^(attempt-1)`.
std::chrono::milliseconds jittered_backoff(
    int attempt,
    std::chrono::milliseconds base = std::chrono::seconds(1),
    std::chrono::milliseconds max_delay = std::chrono::seconds(60),
    double jitter_ratio = 0.25);

}  // namespace hermes::core::retry
