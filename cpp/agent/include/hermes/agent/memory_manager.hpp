// MemoryManager — owns the registered MemoryProviders and fans out
// prefetch / sync calls.  Enforces "at most 1 builtin + at most 1
// external" — providers raise std::invalid_argument when this rule is
// violated.
#pragma once

#include "hermes/agent/memory_provider.hpp"

#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace hermes::agent {

class MemoryManager {
public:
    MemoryManager();
    ~MemoryManager();

    MemoryManager(const MemoryManager&) = delete;
    MemoryManager& operator=(const MemoryManager&) = delete;

    // Throws std::invalid_argument if the new provider would violate
    // the 1-builtin-1-external rule, or if a provider with the same
    // name is already registered.
    void add_provider(std::unique_ptr<MemoryProvider> p);
    void remove_provider(std::string_view name);

    // Concatenate every provider's section, separated by blank lines.
    std::string build_system_prompt();
    void prefetch_all(std::string_view user_message);
    void sync_all(std::string_view user_msg,
                  std::string_view assistant_response);

    // Schedule a prefetch on a background thread.  Existing pending
    // work is replaced (we only care about the latest user message).
    void queue_prefetch_all(std::string_view user_message);

    size_t provider_count() const;

private:
    mutable std::mutex mu_;
    std::vector<std::unique_ptr<MemoryProvider>> providers_;
    std::thread prefetch_thread_;
};

}  // namespace hermes::agent
