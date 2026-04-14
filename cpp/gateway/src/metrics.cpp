#include <hermes/gateway/metrics.hpp>

#include <algorithm>

#include <nlohmann/json.hpp>

namespace hermes::gateway {

MetricsRegistry::MetricsRegistry() = default;
MetricsRegistry::~MetricsRegistry() = default;

void MetricsRegistry::inc(const std::string& name, std::uint64_t delta) {
    std::lock_guard<std::mutex> lock(mu_);
    counters_[name] += delta;
}

void MetricsRegistry::set_gauge(const std::string& name, double value) {
    std::lock_guard<std::mutex> lock(mu_);
    gauges_[name] = value;
}

void MetricsRegistry::add_gauge(const std::string& name, double delta) {
    std::lock_guard<std::mutex> lock(mu_);
    gauges_[name] += delta;
}

std::uint64_t MetricsRegistry::counter(const std::string& name) const {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = counters_.find(name);
    return it == counters_.end() ? 0 : it->second;
}

double MetricsRegistry::gauge(const std::string& name) const {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = gauges_.find(name);
    return it == gauges_.end() ? 0.0 : it->second;
}

std::vector<MetricSample> MetricsRegistry::snapshot() const {
    std::lock_guard<std::mutex> lock(mu_);
    std::unordered_map<std::string, MetricSample> merged;
    for (auto& [k, v] : counters_) merged[k].counter = v;
    for (auto& [k, v] : gauges_) merged[k].gauge = v;
    for (auto& [k, s] : merged) s.name = k;
    std::vector<MetricSample> out;
    out.reserve(merged.size());
    for (auto& [_, s] : merged) out.push_back(s);
    std::sort(out.begin(), out.end(),
               [](const MetricSample& a, const MetricSample& b) {
                   return a.name < b.name;
               });
    return out;
}

nlohmann::json MetricsRegistry::to_json() const {
    auto samples = snapshot();
    nlohmann::json out = nlohmann::json::object();
    for (auto& s : samples) {
        out[s.name] = {{"counter", s.counter}, {"gauge", s.gauge}};
    }
    return out;
}

void MetricsRegistry::reset() {
    std::lock_guard<std::mutex> lock(mu_);
    counters_.clear();
    gauges_.clear();
}

std::vector<std::string> MetricsRegistry::names() const {
    std::lock_guard<std::mutex> lock(mu_);
    std::vector<std::string> out;
    for (auto& [k, _] : counters_) out.push_back(k);
    for (auto& [k, _] : gauges_) out.push_back(k);
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

// --- MetricsEmitter ------------------------------------------------------

MetricsEmitter::MetricsEmitter(MetricsRegistry* registry, Callback on_tick,
                                 std::chrono::milliseconds interval)
    : registry_(registry),
      on_tick_(std::move(on_tick)),
      interval_(interval) {}

MetricsEmitter::~MetricsEmitter() { stop(); }

void MetricsEmitter::start() {
    bool expected = false;
    if (!running_.compare_exchange_strong(expected, true)) return;
    worker_ = std::thread([this] {
        while (running_.load(std::memory_order_acquire)) {
            // Sleep in 100ms slices so stop() unblocks promptly.
            auto slept = std::chrono::milliseconds(0);
            while (slept < interval_ &&
                   running_.load(std::memory_order_acquire)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                slept += std::chrono::milliseconds(100);
            }
            if (!running_.load(std::memory_order_acquire)) break;
            if (registry_ && on_tick_) {
                try {
                    on_tick_(registry_->to_json());
                } catch (...) {
                    // Swallow — emitter must never take the runner down.
                }
            }
        }
    });
}

void MetricsEmitter::stop() {
    if (!running_.exchange(false)) return;
    if (worker_.joinable()) worker_.join();
}

void MetricsEmitter::emit_now() {
    if (registry_ && on_tick_) {
        try {
            on_tick_(registry_->to_json());
        } catch (...) {
        }
    }
}

}  // namespace hermes::gateway
