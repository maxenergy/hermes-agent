// insights — per-session + rolling statistics on agent behaviour.
//
// Tracks tokens in/out, estimated cost, tool usage frequency, and tool
// latency.  Records are buffered in memory and flushed to a simple
// JSONL sidecar at `{HERMES_HOME}/insights.jsonl` (and, where wired,
// a SessionDB-adjacent table).  The intent is a 7-day rolling summary
// surfaced via the `hermes insights` CLI (and `/insights` slash).
//
// Storage rationale: a sidecar JSONL file keeps this module independent
// of the sessions-db schema version, so we can ship migrations
// separately.  A future task can extend SessionDB with a proper
// `insights` table and a v7 migration without breaking this API.
#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace hermes::agent {

// One atomic "event" — either a completed model turn or a tool call.
struct InsightEvent {
    enum class Kind { ModelTurn, ToolCall };

    Kind kind = Kind::ModelTurn;
    std::string session_id;
    std::string model;                  // for ModelTurn
    std::string tool_name;              // for ToolCall
    std::int64_t input_tokens = 0;
    std::int64_t output_tokens = 0;
    double cost_usd = 0.0;
    double latency_ms = 0.0;
    bool error = false;
    std::chrono::system_clock::time_point at{};
};

// Aggregated summary over a set of events.
struct InsightSummary {
    int sessions = 0;
    int model_turns = 0;
    int tool_calls = 0;
    std::int64_t input_tokens = 0;
    std::int64_t output_tokens = 0;
    double cost_usd = 0.0;
    std::map<std::string, int> tool_call_counts;
    std::map<std::string, int> model_turn_counts;
    double latency_p50_ms = 0.0;
    double latency_p95_ms = 0.0;

    // Human-readable multi-line summary suitable for CLI / `/insights`
    // output.
    std::string render() const;
};

class InsightsRecorder {
public:
    // Default path = `{HERMES_HOME}/insights.jsonl`.
    InsightsRecorder();
    explicit InsightsRecorder(std::filesystem::path path);

    // Record a model turn.
    void record_model_turn(const std::string& session_id,
                           const std::string& model,
                           std::int64_t input_tokens,
                           std::int64_t output_tokens,
                           double cost_usd,
                           double latency_ms);

    // Record a tool call.
    void record_tool_call(const std::string& session_id,
                          const std::string& tool_name,
                          double latency_ms,
                          bool error = false);

    // Record a precomposed event.  Exposed for tests / replay.
    void record(const InsightEvent& ev);

    // Parse every line and return events.  Malformed lines are skipped.
    std::vector<InsightEvent> load_all() const;

    // Aggregate helper — events where `at >= cutoff`.
    static InsightSummary summarize(const std::vector<InsightEvent>& events,
                                    std::chrono::system_clock::time_point cutoff);

    // Convenience: load + summarise events from the last `days` days.
    InsightSummary summarize_last_days(int days) const;

    const std::filesystem::path& path() const { return path_; }

private:
    std::filesystem::path path_;
    mutable std::mutex mu_;

    void append_locked_(const std::string& line) const;
};

}  // namespace hermes::agent
