// Trajectory-saving helpers and scratchpad conversion utilities.
//
// C++17 port of agent/trajectory.py. The conversation-to-trajectory
// conversion lives on AIAgent itself; this header exposes the reusable
// helpers that operate on raw assistant content strings.
#pragma once

#include <nlohmann/json.hpp>

#include <string>
#include <vector>

namespace hermes::agent::trajectory {

// Convert <REASONING_SCRATCHPAD>…</REASONING_SCRATCHPAD> pairs into
// <think>…</think> pairs. No-op when the input does not contain the
// scratchpad tags. Preserves surrounding text verbatim.
std::string convert_scratchpad_to_think(const std::string& content);

// Return true when the content contains an opening REASONING_SCRATCHPAD
// tag without a matching closing tag. Used to detect model replies that
// were truncated mid-scratchpad.
bool has_incomplete_scratchpad(const std::string& content);

// Append `entry` as a single JSONL line to `filename`. Returns true on
// success. On failure the error is logged and the function returns false
// (never throws).
//
// When `filename` is empty, the default is picked based on `completed`:
//   completed=true  → "trajectory_samples.jsonl"
//   completed=false → "failed_trajectories.jsonl"
bool save_trajectory(const nlohmann::json& conversations,
                     const std::string& model,
                     bool completed,
                     const std::string& filename = "");

}  // namespace hermes::agent::trajectory

namespace hermes::agent::compression_feedback {

struct ManualCompressionSummary {
    bool noop = false;
    std::string headline;
    std::string token_line;
    std::string note;    // empty when absent
};

// Produce user-facing feedback for a manual /compress command.
ManualCompressionSummary summarize_manual_compression(
    const std::vector<nlohmann::json>& before_messages,
    const std::vector<nlohmann::json>& after_messages,
    long long before_tokens,
    long long after_tokens);

}  // namespace hermes::agent::compression_feedback
