// HuggingFace SFT dataset schema helpers.
//
// A trajectory is an ordered sequence of messages (system / human / gpt /
// tool) captured during an agent conversation.  HuggingFace SFT-style
// datasets (ShareGPT flavour) encode each sample as:
//
//   { "conversations": [ { "from": "<role>", "value": "<text>" }, ... ] }
//
// The mirror on the Python side lives in
// ``run_agent.py::_convert_to_trajectory_format`` — the C++ batch runner
// emits the same XML-wrapped ``<tool_call>`` / ``<tool_response>``
// payloads so SFT records can be combined with the Python trajectories.
#pragma once

#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace hermes::batch {

/// A single turn in a trajectory.  ``from`` is one of
/// ``system`` | ``human`` | ``gpt`` | ``tool``.
struct TrajectoryTurn {
    std::string from;
    std::string value;
};

/// Message roles we accept on the OpenAI side (``role`` field).
struct OpenAIMessage {
    std::string role;      // system | user | assistant | tool
    std::string content;
    std::string reasoning;  // assistant only — optional
    nlohmann::json tool_calls = nlohmann::json::array();  // assistant only
    std::string tool_call_id;  // tool only
};

/// Convert an OpenAI-style message stream + user_query into the
/// HuggingFace SFT ``conversations`` array (each element has ``from`` /
/// ``value`` keys).  ``tools_system_prompt`` is injected as the first
/// ``system`` turn — supply an empty string to skip.
nlohmann::json to_hf_sft_conversations(const std::vector<OpenAIMessage>& messages,
                                        const std::string& user_query,
                                        const std::string& tools_system_prompt);

/// Convenience wrapper: build an SFT record
///   { "conversations": [...], "metadata": {...} }
/// where ``metadata`` is copied verbatim.  Intended for writing one JSON
/// object per line in a JSONL dataset.
nlohmann::json to_hf_sft_record(const std::vector<OpenAIMessage>& messages,
                                 const std::string& user_query,
                                 const std::string& tools_system_prompt,
                                 const nlohmann::json& metadata = nlohmann::json::object());

/// Serialize trajectory turns verbatim (preserving order) into the
/// HuggingFace ``conversations`` shape.  Use this when the caller has
/// already assembled the turns (e.g. legacy pipelines).
nlohmann::json turns_to_conversations(const std::vector<TrajectoryTurn>& turns);

/// Build the standard <tool_call> wrapper for a single tool invocation.
/// ``arguments`` is serialised as JSON object (order-preserving via
/// nlohmann::json).  Mirrors the Python helper in trajectory.py.
std::string format_tool_call(const std::string& name,
                              const nlohmann::json& arguments);

/// Build the standard <tool_response> wrapper for a single tool result.
std::string format_tool_response(const std::string& tool_call_id,
                                  const std::string& name,
                                  const nlohmann::json& content);

/// System-prompt text used for the initial ``from=system`` turn.  Accepts
/// a pre-formatted tool listing (e.g. JSON-serialised schemas joined by
/// newlines).  Pass an empty string to omit the <tools> block.
std::string default_tools_system_prompt(const std::string& formatted_tools);

}  // namespace hermes::batch
