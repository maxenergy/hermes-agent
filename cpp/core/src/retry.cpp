#include "hermes/core/retry.hpp"

#include <algorithm>
#include <cmath>
#include <random>

namespace hermes::core::retry {

namespace {

std::mt19937& thread_rng() {
    thread_local std::mt19937 rng{std::random_device{}()};
    return rng;
}

}  // namespace

std::chrono::milliseconds jittered_backoff(
    int attempt,
    std::chrono::milliseconds base,
    std::chrono::milliseconds max_delay,
    double jitter_ratio) {

    using rep_t = std::chrono::milliseconds::rep;

    if (attempt < 1) {
        attempt = 1;
    }
    // Saturating doubling: 2^(attempt-1). Clamp the shift so the
    // multiplication below never overflows.
    const int shift = std::min(attempt - 1, 30);
    const rep_t multiplier = static_cast<rep_t>(1) << shift;
    const rep_t base_ms = base.count();
    const rep_t max_ms = max_delay.count();

    // Raw growth, capped at max_delay.
    rep_t growth_ms = 0;
    if (base_ms <= 0) {
        growth_ms = 0;
    } else if (multiplier > 0 && base_ms > (std::chrono::milliseconds::max().count() / multiplier)) {
        growth_ms = max_ms;
    } else {
        growth_ms = base_ms * multiplier;
    }
    const rep_t exp_ms = std::min(growth_ms, max_ms);

    // Jitter: uniform on [0, base * jitter_ratio * 2^(attempt-1)].
    double jitter_high = static_cast<double>(base_ms) *
                         jitter_ratio * static_cast<double>(multiplier);
    if (jitter_high < 0.0 || !std::isfinite(jitter_high)) {
        jitter_high = 0.0;
    }
    rep_t jitter_ms = 0;
    if (jitter_high > 0.0) {
        std::uniform_real_distribution<double> dist(0.0, jitter_high);
        jitter_ms = static_cast<rep_t>(std::floor(dist(thread_rng())));
    }

    rep_t total = exp_ms + jitter_ms;
    if (total < 0) {
        total = 0;
    }
    return std::chrono::milliseconds(total);
}

}  // namespace hermes::core::retry
