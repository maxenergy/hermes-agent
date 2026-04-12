// Phase 8: RL training tools — interface to external Nous RL training API.
#pragma once

#include "hermes/llm/llm_client.hpp"

#include <nlohmann/json.hpp>

#include <string>

namespace hermes::tools {

struct RlEnvironment {
    std::string name;
    std::string description;
    std::string version;
};

struct RlTrainingConfig {
    nlohmann::json params;
};

struct RlRunStatus {
    std::string run_id;
    std::string state;
    double progress = 0.0;
    nlohmann::json metrics;
};

void register_rl_tools(hermes::llm::HttpTransport* transport);

}  // namespace hermes::tools
