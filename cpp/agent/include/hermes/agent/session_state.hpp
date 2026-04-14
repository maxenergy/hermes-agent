// Per-session state helpers shared by the agent loop.
//
// Partial C++17 port of agent/session_state.py helpers (cwd/ tool
// counters / turn counters). The SessionDB persistence layer lives in
// hermes_state.
#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace hermes::agent::session_state {

// Per-turn counters that the agent loop increments and the `/usage` and
// `/insights` commands read back. Thread-safe — the delegate subagent
// path mutates these from background threads.
class TurnCounters {
public:
    void record_tool_call(const std::string& tool_name);
    void record_model_turn();
    void record_error();
    void reset();

    std::int64_t tool_call_count() const;
    std::int64_t model_turn_count() const;
    std::int64_t error_count() const;

    // Snapshot of per-tool counts, sorted by count descending.
    struct ToolCount {
        std::string name;
        std::int64_t count = 0;
    };
    std::vector<ToolCount> top_tools(std::size_t limit = 10) const;

private:
    mutable std::mutex mu_;
    std::atomic<std::int64_t> tool_calls_{0};
    std::atomic<std::int64_t> model_turns_{0};
    std::atomic<std::int64_t> errors_{0};
    std::unordered_map<std::string, std::int64_t> per_tool_;
};

// Simple persistent session-id generator mirroring Python uuid hex.
// Caller passes a random seed; the id is a 32-char hex string derived
// deterministically from (seed, counter). Not cryptographically secure
// — used for log correlation, not auth.
std::string make_session_id(std::uint64_t seed);

// Normalise a platform name into the canonical slug used by gateway /
// profile code. "Telegram" → "telegram", "web:room=1" → "web".
std::string normalise_platform_name(const std::string& raw);

}  // namespace hermes::agent::session_state
