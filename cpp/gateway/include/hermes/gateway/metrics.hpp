// Gauge + counter metrics emitted by the gateway runner.
//
// Ports the Python metrics loop that periodically logs active session
// counts / pending queue depth / reconnect attempts / send success rate.
// The C++ port exposes a thread-safe in-memory store plus an emitter
// thread that serializes snapshots to JSON on a tick.
#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <nlohmann/json_fwd.hpp>

namespace hermes::gateway {

// Simple labeled counter / gauge.
struct MetricSample {
    std::string name;
    std::uint64_t counter = 0;  // monotonic
    double gauge = 0.0;          // last observed
};

class MetricsRegistry {
public:
    MetricsRegistry();
    ~MetricsRegistry();

    // Increment the named counter by ``delta``.
    void inc(const std::string& name, std::uint64_t delta = 1);

    // Set the named gauge to ``value``.
    void set_gauge(const std::string& name, double value);

    // Overwrite the named gauge by delta.
    void add_gauge(const std::string& name, double delta);

    // Read a specific counter/gauge.
    std::uint64_t counter(const std::string& name) const;
    double gauge(const std::string& name) const;

    // Complete snapshot, sorted by name.
    std::vector<MetricSample> snapshot() const;

    // Serialize all metrics as a single JSON object {name: {counter,
    // gauge}}.
    nlohmann::json to_json() const;

    // Reset every counter / gauge.  Used by tests.
    void reset();

    // Named list introspection.
    std::vector<std::string> names() const;

private:
    mutable std::mutex mu_;
    std::unordered_map<std::string, std::uint64_t> counters_;
    std::unordered_map<std::string, double> gauges_;
};

// Periodic emitter — pulls snapshots from a MetricsRegistry and passes
// them to a user callback on a fixed interval.  Stops cleanly on
// destruction.  Typical usage wires the callback to
// ``write_runtime_status`` or a JSON log sink.
class MetricsEmitter {
public:
    using Callback = std::function<void(const nlohmann::json&)>;

    MetricsEmitter(MetricsRegistry* registry, Callback on_tick,
                   std::chrono::milliseconds interval);
    ~MetricsEmitter();

    MetricsEmitter(const MetricsEmitter&) = delete;
    MetricsEmitter& operator=(const MetricsEmitter&) = delete;

    void start();
    void stop();

    bool running() const { return running_.load(std::memory_order_acquire); }

    // Force an immediate emission (thread-safe).
    void emit_now();

private:
    MetricsRegistry* registry_;
    Callback on_tick_;
    std::chrono::milliseconds interval_;
    std::atomic<bool> running_{false};
    std::thread worker_;
};

}  // namespace hermes::gateway
