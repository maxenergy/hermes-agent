// Depth port of pure-logic helpers in agent/context_compressor.py.
//
// The existing C++ ContextCompressor focuses on the compression outer
// loop and the token-budget decision. This depth module adds the
// helpers that implement:
//
//   * Tool pair sanitization (orphan tool_result removal + stub
//     injection) — sanitize_tool_pairs()
//   * Head/tail boundary alignment — align_boundary_forward /
//     align_boundary_backward
//   * Token-budget tail cut — find_tail_cut_by_tokens
//   * Tool-result pruning pre-pass — prune_old_tool_results
//   * Summary budget scaling — compute_summary_budget
//   * Serializer for summary model input — serialize_for_summary
//   * Summary prefix normalization — with_summary_prefix
//   * Summary role picker — pick_summary_role

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace hermes::agent::compressor_depth {

// Minimal message record used by the depth helpers. The real runtime
// stores richer information (reasoning content, caching markers, etc.)
// but the helpers only depend on these fields.
struct ToolCall {
    std::string id;
    std::string name;
    std::string arguments;  // JSON-encoded blob
};

struct Message {
    std::string role;          // "system"|"user"|"assistant"|"tool"
    std::string content;
    std::vector<ToolCall> tool_calls;
    std::string tool_call_id;  // only populated when role == "tool"
};

// Public constants mirroring the Python module.
constexpr int kCharsPerToken = 4;
constexpr int kMinSummaryTokens = 2000;
constexpr int kSummaryTokensCeiling = 12000;
constexpr double kSummaryRatio = 0.20;
constexpr int kSummaryFailureCooldownSeconds = 600;

// Public strings so tests can assert exact values.
constexpr std::string_view kSummaryPrefix =
    "[CONTEXT COMPACTION] Earlier turns in this conversation were compacted "
    "to save context space. The summary below describes work that was "
    "already completed, and the current session state may still reflect "
    "that work (for example, files may already be changed). Use the summary "
    "and the current state to continue from where things left off, and "
    "avoid repeating work:";
constexpr std::string_view kLegacySummaryPrefix = "[CONTEXT SUMMARY]:";
constexpr std::string_view kPrunedToolPlaceholder =
    "[Old tool output cleared to save context space]";

// --- Token estimation helpers ---

// Rough per-message token estimate: len(content) / 4 + 10 + args / 4.
std::int64_t estimate_message_tokens_rough(const Message& msg);
std::int64_t estimate_messages_tokens_rough(const std::vector<Message>& msgs);

// --- Summary budget ---

struct SummaryBudgetConfig {
    std::int64_t max_summary_tokens = kSummaryTokensCeiling;
};
std::int64_t compute_summary_budget(const std::vector<Message>& turns,
                                    const SummaryBudgetConfig& cfg = {});

// Scale the max_summary_tokens according to a given context length —
// Python: min(context_length * 0.05, kSummaryTokensCeiling).
std::int64_t derive_max_summary_tokens(std::int64_t context_length);

// Derive the tail token budget: int(threshold * ratio) with ratio
// clamped to [0.10, 0.80].
std::int64_t derive_tail_token_budget(std::int64_t threshold_tokens,
                                      double summary_target_ratio);

// Derive threshold tokens: int(context_length * threshold_percent).
std::int64_t derive_threshold_tokens(std::int64_t context_length,
                                     double threshold_percent);

// Summary-target-ratio clamp: max(0.10, min(ratio, 0.80)).
double clamp_summary_target_ratio(double ratio);

// --- Summary prefix normalization ---

// Python: _with_summary_prefix — strip legacy/current prefix if
// present, then return "<prefix>\n<body>" or the bare prefix on empty
// input.
std::string with_summary_prefix(std::string_view summary);

// --- Tool call / result pair sanitization ---

struct SanitizeReport {
    std::size_t orphan_results_removed = 0;
    std::size_t stubs_inserted = 0;
};
std::vector<Message> sanitize_tool_pairs(
    std::vector<Message> messages,
    SanitizeReport* report = nullptr);

// --- Boundary alignment ---

std::size_t align_boundary_forward(const std::vector<Message>& messages,
                                   std::size_t idx);
std::size_t align_boundary_backward(const std::vector<Message>& messages,
                                    std::size_t idx);

// --- Tail cut by tokens ---

std::size_t find_tail_cut_by_tokens(const std::vector<Message>& messages,
                                    std::size_t head_end,
                                    std::int64_t token_budget);

// --- Old tool result pruning ---

struct PruneReport {
    std::size_t pruned = 0;
};
std::vector<Message> prune_old_tool_results(std::vector<Message> messages,
                                            std::size_t protect_tail_count,
                                            std::int64_t protect_tail_tokens,
                                            PruneReport* report = nullptr);

// --- Serializer for summary model input ---

struct SerializeConfig {
    std::size_t content_max = 6000;
    std::size_t content_head = 4000;
    std::size_t content_tail = 1500;
    std::size_t tool_args_max = 1500;
    std::size_t tool_args_head = 1200;
};
std::string serialize_for_summary(const std::vector<Message>& turns,
                                  const SerializeConfig& cfg = {});

// --- Summary role picker ---

struct SummaryRoleResult {
    std::string role;            // "user" or "assistant"
    bool merge_into_tail = false;
};
SummaryRoleResult pick_summary_role(std::string_view last_head_role,
                                    std::string_view first_tail_role);

// Minimum messages needed before compression is considered worthwhile
// (mirrors the Python check `n_messages <= protect_first_n + 3 + 1`).
bool can_compress(std::size_t n_messages, int protect_first_n);

}  // namespace hermes::agent::compressor_depth
