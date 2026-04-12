// Event hook system for the gateway.
#pragma once

#include <functional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace hermes::gateway {

// Event types
constexpr auto EVT_GATEWAY_STARTUP = "gateway:startup";
constexpr auto EVT_SESSION_START = "session:start";
constexpr auto EVT_SESSION_END = "session:end";
constexpr auto EVT_SESSION_RESET = "session:reset";
constexpr auto EVT_AGENT_START = "agent:start";
constexpr auto EVT_AGENT_STEP = "agent:step";
constexpr auto EVT_AGENT_END = "agent:end";
// command:* wildcard matches command:new, command:reset, etc.

using HookHandler =
    std::function<void(const std::string& event_type,
                       const nlohmann::json& context)>;

class HookRegistry {
public:
    void register_hook(const std::string& event_pattern,
                       HookHandler handler);
    // Emit event — fires all matching handlers.  Never blocks pipeline
    // (exceptions logged).
    void emit(const std::string& event_type,
              const nlohmann::json& context = {});
    void clear();
    size_t size() const;

private:
    struct Entry {
        std::string pattern;
        HookHandler handler;
    };
    std::vector<Entry> hooks_;
    bool matches(const std::string& pattern,
                 const std::string& event_type) const;
};

}  // namespace hermes::gateway
