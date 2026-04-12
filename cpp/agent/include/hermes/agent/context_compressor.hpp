// Built-in ContextEngine implementation: preserves system + first turn
// and the protected tail, replaces the middle with a summarised system
// message produced by an auxiliary LlmClient.
#pragma once

#include "hermes/agent/context_engine.hpp"
#include "hermes/llm/llm_client.hpp"

#include <cstdint>
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

    int compression_count() const { return compression_count_; }
    const CompressionOptions& options() const { return opts_; }

private:
    hermes::llm::LlmClient* summarizer_;
    std::string summarizer_model_;
    CompressionOptions opts_;
    int compression_count_ = 0;
    int64_t current_model_context_ = 0;
};

}  // namespace hermes::agent
