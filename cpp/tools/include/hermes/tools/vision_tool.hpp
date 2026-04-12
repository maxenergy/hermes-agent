// Phase 8: Vision analysis tool — image download + LLM vision call.
#pragma once

#include "hermes/llm/llm_client.hpp"

#include <string>
#include <string_view>

namespace hermes::tools {

// Returns true if the URL points to a private/loopback IP (SSRF guard).
bool is_private_url(std::string_view url);

// Register vision_analyze_tool into the global ToolRegistry.
void register_vision_tools(hermes::llm::HttpTransport* transport);

}  // namespace hermes::tools
