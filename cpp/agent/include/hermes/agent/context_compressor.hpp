// Built-in ContextEngine implementation: preserves system + first turn
// and the protected tail, replaces the middle with a summarised system
// message produced by an auxiliary LlmClient.
//
// Wires the pure-logic helpers from compressor_depth.hpp (port of
// agent/context_compressor.py) into the compress() outer loop:
//   * prune_old_tool_results (cheap pre-pass, no LLM)
//   * align_boundary_forward / align_boundary_backward
//   * find_tail_cut_by_tokens (token-budget tail protection)
//   * serialize_for_summary (summarizer input)
//   * generate_summary (LLM call, iterative on re-compression)
//   * sanitize_tool_pairs (orphan tool_result cleanup)
//   * pick_summary_role (avoids consecutive same-role neighbours)
#pragma once

#include "hermes/agent/context_compressor_depth.hpp"
#include "hermes/agent/context_engine.hpp"
#include "hermes/llm/llm_client.hpp"

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace hermes::agent {

struct CompressionOptions {
    // Fire compression when current_tokens / max_tokens > trigger.
    double trigger_threshold = 0.50;
    // Last N turns (user+assistant+tool messages after a user turn) are
    // never removed.
    int protected_tail_turns = 4;
    // Additional token-based ceiling on the tail.  When walking
    // backwards we stop as soon as the accumulated tail exceeds either
    // the turn count or this token count, whichever is larger.
    int64_t protected_tail_tokens = 20'000;
    // Head messages preserved verbatim (mirrors Python protect_first_n).
    int protect_first_n = 3;
    // Summary ratio used to scale the summariser's output budget (max
    // tokens it may emit).  Clamped to [0.10, 0.80] internally.
    double summary_target_ratio = 0.20;
    // Rendered into the summary system message.  {placeholders} are
    // replaced with extracted fields.  Unrecognised placeholders are
    // left verbatim.
    std::string summary_template =
        "## Compressed history summary\n"
        "**Goal:** {goal}\n"
        "**Progress:** {progress}\n"
        "**Decisions:** {decisions}\n"
        "**Files touched:** {files}\n"
        "**Next steps:** {next_steps}\n";
};

class ContextCompressor : public ContextEngine {
public:
    // `summarizer_client` is non-owning.  A null client disables
    // compression (compress() becomes a no-op).  `summarizer_model` is
    // the model identifier passed to CompletionRequest::model.
    ContextCompressor(hermes::llm::LlmClient* summarizer_client,
                      std::string summarizer_model,
                      CompressionOptions opts = {});

    std::vector<hermes::llm::Message> compress(
        std::vector<hermes::llm::Message> messages,
        int64_t current_tokens,
        int64_t max_tokens) override;

    void on_session_reset() override;
    void update_model(const hermes::llm::ModelMetadata& meta) override;

    std::string_view name() const override { return "compressor"; }

    int compression_count() const { return static_cast<int>(ContextEngine::compression_count); }
    const CompressionOptions& options() const { return opts_; }

    // Visible for tests.
    const std::optional<std::string>& previous_summary() const {
        return previous_summary_;
    }

    // Test hook: override the summary-failure cooldown duration (default
    // 600s, matching agent/context_compressor.py). Passing a zero or
    // negative duration effectively disables the cooldown and allows an
    // immediate retry after a previous failure.
    void set_cooldown_duration(std::chrono::seconds d) {
        cooldown_duration_ = d;
        if (d <= std::chrono::seconds::zero()) {
            // Clear any pending cooldown so the next compress() is free
            // to hit the LLM again.
            summary_failure_cooldown_until_ = std::chrono::steady_clock::time_point{};
        }
    }

    // Test hook: whether generate_summary() is currently suppressed by a
    // prior failure.
    bool in_summary_failure_cooldown() const {
        return std::chrono::steady_clock::now() < summary_failure_cooldown_until_;
    }

private:
    // --- Pipeline stages wired around compressor_depth helpers. ---
    //
    // Each returns a transformed copy and may also mutate the
    // per-instance state (compression count, previous summary).

    // Replace old substantial tool-result contents with a placeholder.
    std::vector<hermes::llm::Message> prune_old_tool_results(
        std::vector<hermes::llm::Message> messages) const;

    // Align the compress-start boundary forward past orphan tool
    // results.
    std::size_t align_cut_forward(
        const std::vector<hermes::llm::Message>& messages,
        std::size_t idx) const;

    // Align the compress-end boundary backward to avoid splitting a
    // tool_call / tool_result group.
    std::size_t align_cut_backward(
        const std::vector<hermes::llm::Message>& messages,
        std::size_t idx) const;

    // Walk backwards from the end accumulating tokens until the budget
    // is hit.
    std::size_t find_tail_cut_by_tokens(
        const std::vector<hermes::llm::Message>& messages,
        std::size_t head_end) const;

    // Build the summariser-facing text block.
    std::string serialize_for_summary(
        const std::vector<hermes::llm::Message>& turns) const;

    // Invoke the summariser LLM.  Returns std::nullopt on any failure.
    std::optional<std::string> generate_summary(
        const std::vector<hermes::llm::Message>& turns);

    hermes::llm::LlmClient* summarizer_;
    std::string summarizer_model_;
    CompressionOptions opts_;
    int64_t current_model_context_ = 0;

    // Stored across compactions so generate_summary() can produce an
    // iterative update rather than summarising from scratch.
    std::optional<std::string> previous_summary_;

    // Summary-failure cooldown — mirrors
    // agent/context_compressor.py::_summary_failure_cooldown_until. If
    // now() < summary_failure_cooldown_until_, generate_summary()
    // short-circuits to std::nullopt (the caller then injects a static
    // marker instead of hitting the LLM). Set to default-constructed
    // (epoch) on success; pushed forward by `cooldown_duration_` on any
    // summariser exception. Default duration is 600s to match Python.
    std::chrono::steady_clock::time_point summary_failure_cooldown_until_{};
    std::chrono::seconds cooldown_duration_{600};
};

}  // namespace hermes::agent
