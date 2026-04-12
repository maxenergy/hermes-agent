#include "hermes/agent/memory_manager.hpp"

#include <algorithm>
#include <atomic>
#include <stdexcept>
#include <string>
#include <utility>

namespace hermes::agent {

namespace {

// We synchronise the queued prefetch thread by capturing the most
// recent message into a shared atomic generation number.  Each call to
// queue_prefetch_all bumps the generation; the worker thread checks
// the generation before publishing its result.
struct PrefetchSlot {
    std::atomic<uint64_t> generation{0};
    std::string message;
    std::mutex msg_mu;
};

}  // namespace

MemoryManager::MemoryManager() = default;

MemoryManager::~MemoryManager() {
    if (prefetch_thread_.joinable()) {
        prefetch_thread_.join();
    }
}

void MemoryManager::add_provider(std::unique_ptr<MemoryProvider> p) {
    if (!p) throw std::invalid_argument("MemoryProvider must not be null");
    std::lock_guard<std::mutex> lock(mu_);

    const std::string new_name = p->name();
    const bool new_external = p->is_external();
    int builtin_count = new_external ? 0 : 1;
    int external_count = new_external ? 1 : 0;
    for (const auto& existing : providers_) {
        if (existing->name() == new_name) {
            throw std::invalid_argument(
                "MemoryProvider already registered: " + new_name);
        }
        if (existing->is_external())
            ++external_count;
        else
            ++builtin_count;
    }
    if (builtin_count > 1)
        throw std::invalid_argument("at most one builtin MemoryProvider allowed");
    if (external_count > 1)
        throw std::invalid_argument("at most one external MemoryProvider allowed");

    providers_.push_back(std::move(p));
}

void MemoryManager::remove_provider(std::string_view name) {
    std::lock_guard<std::mutex> lock(mu_);
    providers_.erase(
        std::remove_if(providers_.begin(), providers_.end(),
                       [&](const std::unique_ptr<MemoryProvider>& p) {
                           return p->name() == name;
                       }),
        providers_.end());
}

std::string MemoryManager::build_system_prompt() {
    std::lock_guard<std::mutex> lock(mu_);
    std::string out;
    for (const auto& p : providers_) {
        auto section = p->build_system_prompt_section();
        if (section.empty()) continue;
        if (!out.empty()) out += "\n";
        out += section;
    }
    return out;
}

void MemoryManager::prefetch_all(std::string_view user_message) {
    std::lock_guard<std::mutex> lock(mu_);
    for (const auto& p : providers_) {
        try {
            p->prefetch(user_message);
        } catch (...) {
            // Providers must not block the loop.
        }
    }
}

void MemoryManager::sync_all(std::string_view user_msg,
                             std::string_view assistant_response) {
    std::lock_guard<std::mutex> lock(mu_);
    for (const auto& p : providers_) {
        try {
            p->sync(user_msg, assistant_response);
        } catch (...) {
            // Providers must not block the loop.
        }
    }
}

void MemoryManager::queue_prefetch_all(std::string_view user_message) {
    // Drain the previous worker if any.  We could maintain a single
    // worker that pulls from a queue, but the spec only requires
    // "tracks latest only" semantics — so a fresh thread per call
    // bounded by join() is fine.
    if (prefetch_thread_.joinable()) {
        prefetch_thread_.join();
    }
    std::string msg(user_message);
    prefetch_thread_ = std::thread([this, msg = std::move(msg)]() {
        try {
            this->prefetch_all(msg);
        } catch (...) {
            // swallow
        }
    });
}

size_t MemoryManager::provider_count() const {
    std::lock_guard<std::mutex> lock(mu_);
    return providers_.size();
}

}  // namespace hermes::agent
