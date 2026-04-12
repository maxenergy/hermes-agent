#include <hermes/gateway/hooks.hpp>

#include <iostream>

namespace hermes::gateway {

void HookRegistry::register_hook(const std::string& event_pattern,
                                 HookHandler handler) {
    hooks_.push_back({event_pattern, std::move(handler)});
}

void HookRegistry::emit(const std::string& event_type,
                        const nlohmann::json& context) {
    for (auto& entry : hooks_) {
        if (matches(entry.pattern, event_type)) {
            try {
                entry.handler(event_type, context);
            } catch (const std::exception& e) {
                // Never block pipeline — log and continue.
                std::cerr << "[hooks] exception in handler for '"
                          << event_type << "': " << e.what() << "\n";
            } catch (...) {
                std::cerr << "[hooks] unknown exception in handler for '"
                          << event_type << "'\n";
            }
        }
    }
}

void HookRegistry::clear() {
    hooks_.clear();
}

size_t HookRegistry::size() const {
    return hooks_.size();
}

bool HookRegistry::matches(const std::string& pattern,
                           const std::string& event_type) const {
    // Exact match.
    if (pattern == event_type) {
        return true;
    }

    // Wildcard: pattern ends with '*' and event_type starts with prefix.
    if (!pattern.empty() && pattern.back() == '*') {
        auto prefix = pattern.substr(0, pattern.size() - 1);
        return event_type.substr(0, prefix.size()) == prefix;
    }

    return false;
}

}  // namespace hermes::gateway
