// MemoryProvider — pluggable memory backends.  Each provider produces a
// fragment that is concatenated into the system prompt, receives
// prefetch hints on every user message, and is asked to sync on every
// completed exchange.
#pragma once

#include "hermes/state/memory_store.hpp"

#include <string>
#include <string_view>

namespace hermes::agent {

class MemoryProvider {
public:
    virtual ~MemoryProvider() = default;

    // Stable short identifier — "builtin", "honcho", "letta", ...
    virtual std::string name() const = 0;
    // True for third-party providers that contact an external service.
    virtual bool is_external() const = 0;

    // Produce the system-prompt section for this provider.  May be
    // empty.  Called on every prompt rebuild.
    virtual std::string build_system_prompt_section() = 0;

    // Fired before an API call with the next user message.  Providers
    // may use this to fetch relevant entries asynchronously.
    virtual void prefetch(std::string_view user_message) = 0;

    // Fired after a completed user→assistant exchange.  Providers may
    // persist new memories here.
    virtual void sync(std::string_view user_msg,
                      std::string_view assistant_response) = 0;
};

// File-backed provider reading from `hermes::state::MemoryStore`.
class BuiltinMemoryProvider : public MemoryProvider {
public:
    explicit BuiltinMemoryProvider(hermes::state::MemoryStore* store);

    std::string name() const override { return "builtin"; }
    bool is_external() const override { return false; }

    std::string build_system_prompt_section() override;
    void prefetch(std::string_view user_message) override;
    void sync(std::string_view user_msg,
              std::string_view assistant_response) override;

private:
    hermes::state::MemoryStore* store_;
};

}  // namespace hermes::agent
