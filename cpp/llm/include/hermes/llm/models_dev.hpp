// models.dev registry fetch stub.  Phase 3 returns nullopt; Phase 4 will
// wire the real HTTP fetch with caching.
#pragma once

#include "hermes/llm/model_metadata.hpp"

#include <chrono>
#include <optional>
#include <string_view>

namespace hermes::llm::models_dev {

// TODO(phase-4): real HTTP fetch against models.dev with `cache_ttl`.
std::optional<ModelMetadata> fetch_spec(
    std::string_view model,
    std::chrono::seconds cache_ttl = std::chrono::seconds(3600));

}  // namespace hermes::llm::models_dev
