// Phase 8: Image generation tool — DALL-E / Flux / Ideogram / Replicate.
#pragma once

#include "hermes/llm/llm_client.hpp"

namespace hermes::tools {

void register_image_gen_tools(hermes::llm::HttpTransport* transport);

// Test hooks for the 1-hour model-list TTL cache.
void clear_image_model_cache();
void set_image_model_cache_ttl_seconds(int ttl);

}  // namespace hermes::tools
