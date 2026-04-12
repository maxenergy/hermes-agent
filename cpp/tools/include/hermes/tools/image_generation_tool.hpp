// Phase 8: Image generation tool — DALL-E / Flux / Ideogram.
#pragma once

#include "hermes/llm/llm_client.hpp"

namespace hermes::tools {

void register_image_gen_tools(hermes::llm::HttpTransport* transport);

}  // namespace hermes::tools
