// C++17 port of agent/context_engine.py default methods.
#include "hermes/agent/context_engine.hpp"

#include <algorithm>

namespace hermes::agent {

nlohmann::json ContextEngineStatus::to_json() const {
    return nlohmann::json{
        {"last_prompt_tokens", last_prompt_tokens},
        {"threshold_tokens", threshold_tokens},
        {"context_length", context_length},
        {"usage_percent", usage_percent},
        {"compression_count", compression_count},
    };
}

void ContextEngine::update_from_response(const nlohmann::json& usage) {
    if (!usage.is_object()) return;
    auto read = [&usage](const char* a, const char* b = nullptr) -> std::int64_t {
        auto it = usage.find(a);
        if (it != usage.end() && it->is_number_integer()) {
            return it->get<std::int64_t>();
        }
        if (b != nullptr) {
            it = usage.find(b);
            if (it != usage.end() && it->is_number_integer()) {
                return it->get<std::int64_t>();
            }
        }
        return 0;
    };
    // Prefer canonical names; fall back to vendor-specific aliases.
    last_prompt_tokens = read("prompt_tokens", "input_tokens");
    last_completion_tokens = read("completion_tokens", "output_tokens");
    last_total_tokens = read("total_tokens");
    if (last_total_tokens == 0) {
        last_total_tokens = last_prompt_tokens + last_completion_tokens;
    }
}

bool ContextEngine::should_compress(std::int64_t prompt_tokens_override) const {
    const std::int64_t pt = prompt_tokens_override >= 0
                                ? prompt_tokens_override
                                : last_prompt_tokens;
    if (threshold_tokens <= 0) return false;
    return pt >= threshold_tokens;
}

bool ContextEngine::should_compress_preflight(
    const std::vector<hermes::llm::Message>&) const {
    return false;
}

void ContextEngine::on_session_start(const std::string& /*session_id*/) {}

void ContextEngine::on_session_end(
    const std::string& /*session_id*/,
    const std::vector<hermes::llm::Message>& /*messages*/) {}

std::vector<nlohmann::json> ContextEngine::get_tool_schemas() const {
    return {};
}

std::string ContextEngine::handle_tool_call(const std::string& name,
                                            const nlohmann::json& /*args*/) {
    nlohmann::json err = {
        {"error", "Unknown context engine tool: " + name},
    };
    return err.dump();
}

ContextEngineStatus ContextEngine::get_status() const {
    ContextEngineStatus s;
    s.last_prompt_tokens = last_prompt_tokens;
    s.threshold_tokens = threshold_tokens;
    s.context_length = context_length;
    s.compression_count = compression_count;
    if (context_length > 0) {
        double pct = static_cast<double>(last_prompt_tokens) /
                     static_cast<double>(context_length) * 100.0;
        s.usage_percent = std::min(100.0, pct);
    } else {
        s.usage_percent = 0.0;
    }
    return s;
}

}  // namespace hermes::agent
