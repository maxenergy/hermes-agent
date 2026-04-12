// models.dev registry fetch. Returns nullopt when the remote registry
// is unreachable; uses local cache with the given TTL.
#pragma once

#include "hermes/llm/model_metadata.hpp"

#include <chrono>
#include <optional>
#include <string_view>

namespace hermes::llm::models_dev {

// Fetch model metadata from models.dev with `cache_ttl`.
std::optional<ModelMetadata> fetch_spec(
    std::string_view model,
    std::chrono::seconds cache_ttl = std::chrono::seconds(3600));

}  // namespace hermes::llm::models_dev
