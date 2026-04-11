// models.dev stub — always returns nullopt in Phase 3.
#include "hermes/llm/models_dev.hpp"

namespace hermes::llm::models_dev {

std::optional<ModelMetadata> fetch_spec(std::string_view /*model*/,
                                        std::chrono::seconds /*cache_ttl*/) {
    // TODO(phase-4): implement real HTTP fetch + cache.
    return std::nullopt;
}

}  // namespace hermes::llm::models_dev
