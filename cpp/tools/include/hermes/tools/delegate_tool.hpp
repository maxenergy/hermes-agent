// Phase 8: Delegate + Mixture-of-Agents tool stubs.
#pragma once

#include <functional>
#include <memory>
#include <string>

namespace hermes::tools {

// Forward-declared agent interface for Phase 12 wiring.
class AIAgent {
public:
    virtual ~AIAgent() = default;
    virtual std::string run(const std::string& goal,
                            const std::string& constraints) = 0;
};

// Factory that creates a sub-agent.  nullptr ⇒ delegate not wired.
using AgentFactory = std::function<std::unique_ptr<AIAgent>(const std::string& model)>;

void register_delegate_tools(AgentFactory factory = nullptr);

}  // namespace hermes::tools
