#include "hermes/agent/memory_provider.hpp"

namespace hermes::agent {

BuiltinMemoryProvider::BuiltinMemoryProvider(hermes::state::MemoryStore* store)
    : store_(store) {}

std::string BuiltinMemoryProvider::build_system_prompt_section() {
    if (!store_) return {};
    std::string out;
    auto agent = store_->read_all(hermes::state::MemoryFile::Agent);
    auto user = store_->read_all(hermes::state::MemoryFile::User);
    if (!agent.empty()) {
        out += "### MEMORY.md\n";
        for (const auto& e : agent) {
            out += "- ";
            out += e;
            out += '\n';
        }
    }
    if (!user.empty()) {
        out += "### USER.md\n";
        for (const auto& e : user) {
            out += "- ";
            out += e;
            out += '\n';
        }
    }
    return out;
}

void BuiltinMemoryProvider::prefetch(std::string_view) {
    // The file-backed provider has nothing to prefetch — every read
    // hits the local filesystem and is fast enough for the prompt
    // builder to call inline.
}

void BuiltinMemoryProvider::sync(std::string_view, std::string_view) {
    // The model issues explicit `memory` tool calls when it wants to
    // write something — there is no implicit sync.
}

}  // namespace hermes::agent
