// C++17 port of agent/memory_provider.py default implementations, plus
// the file-backed BuiltinMemoryProvider.
#include "hermes/agent/memory_provider.hpp"

#include <stdexcept>
#include <string>
#include <utility>

namespace hermes::agent {

// ── Default MemoryProvider implementations ────────────────────────────

void MemoryProvider::initialize(const MemoryProviderContext&) {
    // Default: nothing to set up.
}

void MemoryProvider::shutdown() {
    // Default: nothing to tear down.
}

std::string MemoryProvider::prefetch_string(const std::string&,
                                            const std::string&) {
    return {};
}

void MemoryProvider::queue_prefetch(const std::string&,
                                    const std::string&) {
    // No-op default; background-prefetching providers override.
}

void MemoryProvider::sync(std::string_view, std::string_view) {
    // Legacy fire-and-forget sync; default no-op. Modern providers route
    // through sync_turn instead.
}

void MemoryProvider::sync_turn(const std::string& user_content,
                               const std::string& assistant_content,
                               const std::string& /*session_id*/) {
    sync(user_content, assistant_content);
}

std::vector<nlohmann::json> MemoryProvider::get_tool_schemas() const {
    return {};
}

std::string MemoryProvider::handle_tool_call(const std::string& tool_name,
                                             const nlohmann::json&,
                                             const MemoryProviderContext&) {
    nlohmann::json err = {
        {"error", "Provider " + this->name() +
                      " does not handle tool " + tool_name},
    };
    return err.dump();
}

void MemoryProvider::on_turn_start(int, const std::string&,
                                   const MemoryProviderContext&) {}

void MemoryProvider::on_session_end(
    const std::vector<hermes::llm::Message>&,
    const MemoryProviderContext&) {}

std::string MemoryProvider::on_pre_compress(
    const std::vector<hermes::llm::Message>&) {
    return {};
}

void MemoryProvider::on_delegation(const std::string&,
                                   const std::string&,
                                   const std::string&,
                                   const MemoryProviderContext&) {}

void MemoryProvider::on_memory_write(const std::string&,
                                     const std::string&,
                                     const std::string&) {}

std::vector<MemoryConfigField> MemoryProvider::get_config_schema() const {
    return {};
}

void MemoryProvider::save_config(const nlohmann::json&, const std::string&) {
    // Default: env-var-only providers need no on-disk config file.
}

// ── BuiltinMemoryProvider ─────────────────────────────────────────────

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
    // File-backed store has nothing to prefetch — every read hits the
    // local filesystem and is fast enough for inline use.
}

std::string BuiltinMemoryProvider::prefetch_string(const std::string&,
                                                   const std::string&) {
    return {};
}

void BuiltinMemoryProvider::sync(std::string_view, std::string_view) {
    // The model issues explicit `memory` tool calls when it wants to
    // write something — there is no implicit sync.
}

}  // namespace hermes::agent
