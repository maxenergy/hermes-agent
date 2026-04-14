// Jittered exponential backoff for retry paths.
//
// C++17 port of agent/retry_utils.py. Decorrelates concurrent retries by
// mixing a time-based seed with a per-process monotonic tick into the
// jitter RNG, so multiple gateway sessions hitting the same provider
// do not all retry at the same instant.
#pragma once

#include <cstddef>

namespace hermes::agent {

// Compute a jittered exponential backoff delay in seconds.
//
// attempt: 1-based retry attempt number.
// base_delay: base delay (seconds) for attempt 1.
// max_delay: cap (seconds) on the exponential component.
// jitter_ratio: fraction of the computed delay to use as random jitter
//     range (uniform in [0, jitter_ratio * delay]).
//
// Returns: min(base * 2^(attempt-1), max_delay) + uniform(0, ratio*delay).
double jittered_backoff(
    int attempt,
    double base_delay = 5.0,
    double max_delay = 120.0,
    double jitter_ratio = 0.5);

// Expose the internal counter for tests (deterministic seeding across runs
// is infeasible — we only validate that the delay falls in the expected
// range).
std::size_t jitter_tick_count_for_testing();

}  // namespace hermes::agent
