// Abstract ContextEngine — pluggable context management.
#pragma once

#include "hermes/llm/message.hpp"
#include "hermes/llm/model_metadata.hpp"

#include <cstdint>
#include <vector>

namespace hermes::agent {

// Engines decide how (and when) to compact the running message list.
// The built-in ContextCompressor is the default implementation.
class ContextEngine {
public:
    virtual ~ContextEngine() = default;

    // Given the current conversation history plus live token counts,
    // return a (possibly shorter) message list that fits under the
    // engine's budget.  Engines MUST preserve the system message and
    // the protected tail (see implementation docs).
    virtual std::vector<hermes::llm::Message> compress(
        std::vector<hermes::llm::Message> messages,
        int64_t current_tokens,
        int64_t max_tokens) = 0;

    // Called on /reset or /new.
    virtual void on_session_reset() = 0;

    // Called when the active model changes (e.g. CLI /model).
    virtual void update_model(const hermes::llm::ModelMetadata& meta) = 0;
};

}  // namespace hermes::agent
