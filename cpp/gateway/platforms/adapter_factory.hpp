// Phase 12 — Platform adapter factory.
#pragma once

#include <memory>
#include <vector>

#include <hermes/gateway/gateway_config.hpp>
#include <hermes/gateway/gateway_runner.hpp>

namespace hermes::gateway::platforms {

// Create an adapter for the given platform, configured from PlatformConfig.
std::unique_ptr<BasePlatformAdapter> create_adapter(Platform p,
                                                     const PlatformConfig& cfg);

// Return all Platform enum values that have adapter implementations.
std::vector<Platform> available_adapters();

}  // namespace hermes::gateway::platforms
