// Abstract ContextEngine — pluggable context management.
//
// C++17 port of agent/context_engine.py. Engines decide how (and when)
// to compact the running message list. Third-party engines (e.g. LCM)
// plug in via the plugin loader. Selection is config-driven:
// `context.engine` in config.yaml. Default is "compressor".
//
// Lifecycle:
//   1. Engine is instantiated and registered.
//   2. on_session_start() called when a conversation begins.
//   3. update_from_response() called after each API response.
//   4. should_compress() checked after each turn.
//   5. compress() called when should_compress() returns true.
//   6. on_session_end() called at real session boundaries (CLI exit,
//      /reset, gateway session expiry) — NOT per-turn.
#pragma once

#include "hermes/llm/message.hpp"
#include "hermes/llm/model_metadata.hpp"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace hermes::agent {

// Optional engine status dict (mirrors Python get_status()).
struct ContextEngineStatus {
    std::int64_t last_prompt_tokens = 0;
    std::int64_t threshold_tokens = 0;
    std::int64_t context_length = 0;
    double usage_percent = 0.0;
    std::int64_t compression_count = 0;

    nlohmann::json to_json() const;
};

class ContextEngine {
public:
    virtual ~ContextEngine() = default;

    // -- Identity --
    virtual std::string_view name() const = 0;

    // -- Required: core mutation API --
    virtual std::vector<hermes::llm::Message> compress(
        std::vector<hermes::llm::Message> messages,
        int64_t current_tokens,
        int64_t max_tokens) = 0;

    virtual void on_session_reset() = 0;
    virtual void update_model(const hermes::llm::ModelMetadata& meta) = 0;

    // -- Optional: response-driven state updates --
    virtual void update_from_response(const nlohmann::json& usage);

    // -- Optional: compaction predicates --
    virtual bool should_compress(std::int64_t prompt_tokens_override = -1) const;
    virtual bool should_compress_preflight(
        const std::vector<hermes::llm::Message>& messages) const;

    // -- Optional: session lifecycle --
    virtual void on_session_start(const std::string& session_id);
    virtual void on_session_end(
        const std::string& session_id,
        const std::vector<hermes::llm::Message>& messages);

    // -- Optional: tools --
    virtual std::vector<nlohmann::json> get_tool_schemas() const;
    virtual std::string handle_tool_call(const std::string& name,
                                         const nlohmann::json& args);

    // -- Status --
    virtual ContextEngineStatus get_status() const;

    // Engine-wide parameters (read by run_agent loop).
    std::int64_t last_prompt_tokens = 0;
    std::int64_t last_completion_tokens = 0;
    std::int64_t last_total_tokens = 0;
    std::int64_t threshold_tokens = 0;
    std::int64_t context_length = 0;
    std::int64_t compression_count = 0;

    double threshold_percent = 0.75;
    int protect_first_n = 3;
    int protect_last_n = 6;
};

}  // namespace hermes::agent
