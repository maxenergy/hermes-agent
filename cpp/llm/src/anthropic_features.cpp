// Anthropic request/response feature helpers — implementation.
//
// See anthropic_features.hpp for per-function contracts.  These helpers
// are pure (no I/O), so the whole file can be fuzzed / covered by
// deterministic unit tests.
#include "hermes/llm/anthropic_features.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <string>
#include <unordered_set>

namespace hermes::llm {

using nlohmann::json;

namespace {

std::string to_lower_copy(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        out.push_back(static_cast<char>(
            std::tolower(static_cast<unsigned char>(c))));
    }
    return out;
}

bool contains_ci(std::string_view hay, std::string_view needle) {
    if (needle.empty()) return true;
    if (hay.size() < needle.size()) return false;
    for (std::size_t i = 0; i + needle.size() <= hay.size(); ++i) {
        bool ok = true;
        for (std::size_t j = 0; j < needle.size(); ++j) {
            const char a = static_cast<char>(
                std::tolower(static_cast<unsigned char>(hay[i + j])));
            const char b = static_cast<char>(
                std::tolower(static_cast<unsigned char>(needle[j])));
            if (a != b) { ok = false; break; }
        }
        if (ok) return true;
    }
    return false;
}

}  // namespace

// ── tool_choice ─────────────────────────────────────────────────────────

std::optional<json> map_tool_choice_to_anthropic(std::string_view choice) {
    if (choice.empty()) {
        json j;
        j["type"] = "auto";
        return j;
    }
    const std::string lower = to_lower_copy(choice);
    if (lower == "auto") {
        json j; j["type"] = "auto"; return j;
    }
    if (lower == "required" || lower == "any") {
        json j; j["type"] = "any"; return j;
    }
    if (lower == "none") {
        return std::nullopt;
    }
    // Specific tool name — preserve original casing.
    json j;
    j["type"] = "tool";
    j["name"] = std::string(choice);
    return j;
}

// ── thinking ────────────────────────────────────────────────────────────

int thinking_budget_for_effort(std::string_view effort) {
    const std::string e = to_lower_copy(effort);
    if (e == "minimal" || e == "min")       return 2048;
    if (e == "low")                         return 2048;
    if (e == "medium" || e == "med" || e.empty()) return 8000;
    if (e == "high")                        return 16000;
    if (e == "maximum" || e == "max")       return 32000;
    return 8000;
}

std::string_view map_adaptive_effort(std::string_view effort) {
    const std::string e = to_lower_copy(effort);
    if (e == "minimal" || e == "min") return "minimal";
    if (e == "low")                   return "low";
    if (e == "high")                  return "high";
    if (e == "maximum" || e == "max") return "high";  // Anthropic caps at high
    return "medium";
}

bool supports_adaptive_thinking(std::string_view model) {
    // Claude 4.6 family — Opus/Sonnet/Haiku 4.6.  Haiku 4.5 still qualifies
    // for adaptive thinking type detection, but extended thinking is
    // gated separately by supports_extended_thinking.
    const std::string m = to_lower_copy(model);
    return contains_ci(m, "-4-6") ||
           contains_ci(m, "-4.6") ||
           contains_ci(m, "opus-4-6") ||
           contains_ci(m, "sonnet-4-6");
}

bool supports_extended_thinking(std::string_view model) {
    // Haiku does NOT support extended thinking on any version.
    if (contains_ci(model, "haiku")) return false;
    return contains_ci(model, "claude");
}

json build_thinking_config(std::string_view model,
                           std::string_view effort,
                           int current_max_tokens) {
    json out = json::object();
    if (!supports_extended_thinking(model)) return out;
    const int budget = thinking_budget_for_effort(effort);

    if (supports_adaptive_thinking(model)) {
        out["thinking"] = {{"type", "adaptive"}};
        out["output_config"] = {{"effort", std::string(map_adaptive_effort(effort))}};
    } else {
        out["thinking"] = {{"type", "enabled"}, {"budget_tokens", budget}};
        out["temperature"] = 1;
        out["max_tokens"] = std::max(current_max_tokens, budget + 4096);
    }
    return out;
}

// ── stop_reason ─────────────────────────────────────────────────────────

std::string map_anthropic_stop_reason(std::string_view r) {
    const std::string s = to_lower_copy(r);
    if (s == "end_turn" || s == "stop_sequence" || s == "pause_turn") return "stop";
    if (s == "tool_use") return "tool_calls";
    if (s == "max_tokens") return "length";
    if (s == "refusal") return "content_filter";
    if (s.empty()) return "stop";
    return "stop";
}

// ── request extras ──────────────────────────────────────────────────────

AnthropicRequestExtras parse_anthropic_extras(const json& extra) {
    AnthropicRequestExtras out;
    if (!extra.is_object()) return out;

    if (extra.contains("stop_sequences") && extra["stop_sequences"].is_array()) {
        for (const auto& s : extra["stop_sequences"]) {
            if (s.is_string()) out.stop_sequences.push_back(s.get<std::string>());
        }
    }
    if (extra.contains("top_p") && extra["top_p"].is_number()) {
        out.top_p = extra["top_p"].get<double>();
    }
    if (extra.contains("top_k") && extra["top_k"].is_number_integer()) {
        out.top_k = extra["top_k"].get<int>();
    }
    if (extra.contains("service_tier") && extra["service_tier"].is_string()) {
        out.service_tier = extra["service_tier"].get<std::string>();
    }
    if (extra.contains("tool_choice") && extra["tool_choice"].is_string()) {
        out.tool_choice = extra["tool_choice"].get<std::string>();
    }
    if (extra.contains("thinking_effort") && extra["thinking_effort"].is_string()) {
        out.thinking_effort = extra["thinking_effort"].get<std::string>();
    }
    if (extra.contains("fast_mode") && extra["fast_mode"].is_boolean()) {
        out.fast_mode = extra["fast_mode"].get<bool>();
    }
    if (extra.contains("is_oauth") && extra["is_oauth"].is_boolean()) {
        out.is_oauth = extra["is_oauth"].get<bool>();
    }
    return out;
}

// ── beta headers ────────────────────────────────────────────────────────

bool is_third_party_anthropic_endpoint(std::string_view base_url) {
    if (base_url.empty()) return false;
    const std::string u = to_lower_copy(base_url);
    if (contains_ci(u, "api.anthropic.com")) return false;
    if (contains_ci(u, "anthropic.com"))     return false;
    // Known third-party Anthropic-compatible endpoints.
    static constexpr std::array<const char*, 9> kThirdParty = {
        "z.ai", "zhipu", "bigmodel",
        "moonshot", "kimi",
        "minimax",
        "dashscope", "aliyuncs",
        "openrouter",
    };
    for (const char* t : kThirdParty) {
        if (contains_ci(u, t)) return true;
    }
    // Anything not Anthropic's native base is assumed third-party.
    return true;
}

std::vector<std::string> common_betas_for_base_url(std::string_view base_url) {
    std::vector<std::string> betas;
    // interleaved-thinking is supported by both native and most third-party
    // Anthropic-compatible endpoints.
    betas.emplace_back("interleaved-thinking-2025-05-14");
    // fine-grained-tool-streaming is native only.
    if (!is_third_party_anthropic_endpoint(base_url)) {
        betas.emplace_back("fine-grained-tool-streaming-2025-05-14");
    }
    return betas;
}

// ── OAuth sanitization ──────────────────────────────────────────────────

namespace {
std::string replace_all(std::string_view haystack,
                        std::string_view needle,
                        std::string_view repl) {
    if (needle.empty()) return std::string(haystack);
    std::string out;
    out.reserve(haystack.size());
    std::size_t i = 0;
    while (i < haystack.size()) {
        if (i + needle.size() <= haystack.size() &&
            haystack.compare(i, needle.size(), needle) == 0) {
            out.append(repl);
            i += needle.size();
        } else {
            out.push_back(haystack[i++]);
        }
    }
    return out;
}
}  // namespace

std::string sanitize_for_claude_code_oauth(std::string_view input) {
    std::string s(input);
    s = replace_all(s, "Hermes Agent", "Claude Code");
    s = replace_all(s, "Hermes agent", "Claude Code");
    s = replace_all(s, "hermes-agent", "claude-code");
    s = replace_all(s, "Nous Research", "Anthropic");
    return s;
}

std::string apply_mcp_tool_prefix(std::string_view name) {
    static constexpr const char* kPrefix = "mcp_";
    if (name.size() >= 4 && name.substr(0, 4) == "mcp_") {
        return std::string(name);
    }
    std::string out;
    out.reserve(name.size() + 4);
    out.append(kPrefix);
    out.append(name);
    return out;
}

std::string strip_mcp_tool_prefix(std::string_view name) {
    if (name.size() >= 4 && name.substr(0, 4) == "mcp_") {
        return std::string(name.substr(4));
    }
    return std::string(name);
}

// ── reasoning extraction ────────────────────────────────────────────────

ExtractedReasoning extract_reasoning_blocks(const json& content_array) {
    ExtractedReasoning out;
    if (!content_array.is_array()) return out;
    for (const auto& block : content_array) {
        if (!block.is_object()) continue;
        const auto type = block.value("type", std::string{});
        if (type == "thinking") {
            if (!out.text.empty()) out.text += "\n\n";
            out.text += block.value("thinking", std::string{});
            out.blocks.push_back(block);
            if (block.contains("signature")) out.has_signature = true;
        } else if (type == "redacted_thinking") {
            out.blocks.push_back(block);
            if (block.contains("signature")) out.has_signature = true;
        }
    }
    return out;
}

// ── stop sequences ──────────────────────────────────────────────────────

std::vector<std::string> normalize_stop_sequences(
    const std::vector<std::string>& raw) {
    std::vector<std::string> out;
    std::unordered_set<std::string> seen;
    for (const auto& s : raw) {
        if (s.empty()) continue;
        if (seen.insert(s).second) {
            out.push_back(s);
            if (out.size() == 4) break;
        }
    }
    return out;
}

// ── breakpoint inspector ────────────────────────────────────────────────

namespace {
bool message_has_marker(const Message& m) {
    if (m.cache_control.has_value()) return true;
    for (const auto& b : m.content_blocks) {
        if (b.cache_control.has_value()) return true;
    }
    return false;
}
}  // namespace

CacheBreakpointInfo inspect_cache_breakpoints(
    const std::vector<Message>& messages) {
    CacheBreakpointInfo info;
    for (std::size_t i = 0; i < messages.size(); ++i) {
        if (message_has_marker(messages[i])) {
            info.breakpoint_indices.push_back(static_cast<int>(i));
            ++info.total_breakpoints;
            if (messages[i].role == Role::System) ++info.system_breakpoints;
            else ++info.message_breakpoints;
        }
    }
    return info;
}

}  // namespace hermes::llm
