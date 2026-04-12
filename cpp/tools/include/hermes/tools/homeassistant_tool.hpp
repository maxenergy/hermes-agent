// Home Assistant tools — entity state queries and service calls.
// NOTE: HTTP transport is not yet wired (requires cpr, Phase 9).
#pragma once

#include "hermes/tools/registry.hpp"

namespace hermes::tools {

void register_homeassistant_tools(ToolRegistry& registry);

}  // namespace hermes::tools
