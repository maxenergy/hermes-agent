#include "hermes/agent/session_state.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <random>
#include <string>
#include <vector>

namespace hermes::agent::session_state {

// ── TurnCounters ────────────────────────────────────────────────────

void TurnCounters::record_tool_call(const std::string& tool_name) {
    tool_calls_.fetch_add(1, std::memory_order_relaxed);
    std::lock_guard<std::mutex> lock(mu_);
    ++per_tool_[tool_name];
}

void TurnCounters::record_model_turn() {
    model_turns_.fetch_add(1, std::memory_order_relaxed);
}

void TurnCounters::record_error() {
    errors_.fetch_add(1, std::memory_order_relaxed);
}

void TurnCounters::reset() {
    tool_calls_.store(0, std::memory_order_relaxed);
    model_turns_.store(0, std::memory_order_relaxed);
    errors_.store(0, std::memory_order_relaxed);
    std::lock_guard<std::mutex> lock(mu_);
    per_tool_.clear();
}

std::int64_t TurnCounters::tool_call_count() const {
    return tool_calls_.load(std::memory_order_relaxed);
}

std::int64_t TurnCounters::model_turn_count() const {
    return model_turns_.load(std::memory_order_relaxed);
}

std::int64_t TurnCounters::error_count() const {
    return errors_.load(std::memory_order_relaxed);
}

std::vector<TurnCounters::ToolCount> TurnCounters::top_tools(std::size_t limit) const {
    std::lock_guard<std::mutex> lock(mu_);
    std::vector<ToolCount> out;
    out.reserve(per_tool_.size());
    for (const auto& [name, count] : per_tool_) {
        out.push_back({name, count});
    }
    std::sort(out.begin(), out.end(),
              [](const ToolCount& a, const ToolCount& b) {
                  if (a.count != b.count) return a.count > b.count;
                  return a.name < b.name;
              });
    if (out.size() > limit) out.resize(limit);
    return out;
}

// ── Session id ─────────────────────────────────────────────────────

std::string make_session_id(std::uint64_t seed) {
    // Combine seed + local counter to make multi-call sequences stable
    // within a run but distinct across runs.
    static std::atomic<std::uint64_t> g_counter{0};
    const std::uint64_t tick = g_counter.fetch_add(1, std::memory_order_relaxed);
    std::mt19937_64 rng(seed ^ (tick * 0x9E3779B97F4A7C15ULL));
    std::uniform_int_distribution<std::uint64_t> dist;
    const std::uint64_t hi = dist(rng);
    const std::uint64_t lo = dist(rng);
    char buf[33];
    std::snprintf(buf, sizeof(buf), "%016llx%016llx",
                  static_cast<unsigned long long>(hi),
                  static_cast<unsigned long long>(lo));
    return std::string(buf);
}

// ── Platform normalisation ─────────────────────────────────────────

std::string normalise_platform_name(const std::string& raw) {
    std::string lowered;
    lowered.reserve(raw.size());
    for (char c : raw) {
        unsigned char u = static_cast<unsigned char>(c);
        if (std::isspace(u)) continue;
        if (c == ':' || c == ',') break;  // strip suffix (web:room=1)
        lowered.push_back(static_cast<char>(std::tolower(u)));
    }
    // Strip trailing punctuation.
    while (!lowered.empty() &&
           !std::isalnum(static_cast<unsigned char>(lowered.back()))) {
        lowered.pop_back();
    }
    return lowered;
}

}  // namespace hermes::agent::session_state
