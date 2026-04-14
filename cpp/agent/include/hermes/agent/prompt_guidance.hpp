// Model- and platform-specific system-prompt guidance blocks.
//
// C++17 port of the guidance constants from agent/prompt_builder.py
// (TOOL_USE_ENFORCEMENT_GUIDANCE, OPENAI_MODEL_EXECUTION_GUIDANCE,
// GOOGLE_MODEL_OPERATIONAL_GUIDANCE, DEVELOPER_ROLE_MODELS) plus the
// context-file threat scanner. Kept separate from prompt_builder.hpp
// so unrelated headers (gateway adapters) can consume the guidance
// without pulling in the larger PromptBuilder machinery.
#pragma once

#include <string>
#include <string_view>

namespace hermes::agent::guidance {

// Large multi-line constants. Defined in prompt_guidance.cpp.
extern const std::string TOOL_USE_ENFORCEMENT;
extern const std::string OPENAI_MODEL_EXECUTION;
extern const std::string GOOGLE_MODEL_OPERATIONAL;

// Returns true when `model` matches a family that needs tool-use
// enforcement steering (GPT / Codex / Gemini / Gemma / Grok).
bool model_needs_tool_use_enforcement(const std::string& model);

// True when `model` is an OpenAI GPT / Codex model family.
bool model_is_openai(const std::string& model);

// True when `model` is a Google Gemini / Gemma family.
bool model_is_google(const std::string& model);

// True when the system prompt for `model` should use the "developer"
// role instead of "system" (OpenAI's GPT-5 / Codex family).
bool model_uses_developer_role(const std::string& model);

// Concatenate the applicable guidance blocks for `model` into a single
// string separated by blank lines. Returns empty string when none apply.
std::string select_guidance_for_model(const std::string& model);

}  // namespace hermes::agent::guidance

namespace hermes::agent::context_scanner {

struct ScanResult {
    bool blocked = false;
    std::string reason;      // comma-joined findings when blocked
    std::string output;      // sanitized content (original when safe)
};

// Scan a context file body for prompt-injection threats. Returns a
// ScanResult with `blocked=true` and a placeholder `output` when any
// threat pattern or invisible unicode character is detected.
ScanResult scan_context_content(const std::string& content,
                                const std::string& filename);

}  // namespace hermes::agent::context_scanner
