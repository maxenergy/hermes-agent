// Helpers for the tool-dispatch wrapper layer.
//
// Partial port of agent/tool_dispatch_wrapper.py — the pure pieces that
// decorate a raw tool result string (timing, truncation, redaction
// coordination) before it is attached back to the conversation.
#pragma once

#include <cstddef>
#include <string>

namespace hermes::agent::tool_dispatch {

// Maximum bytes to retain in a tool-result string before truncating.
// Matches Python's TOOL_RESULT_MAX_BYTES.
constexpr std::size_t kMaxBytes = 200000;

// Truncate `content` to `max_bytes`, appending a truncation marker that
// reports how many bytes were dropped. No-op when content already fits.
std::string truncate_tool_result(const std::string& content,
                                 std::size_t max_bytes = kMaxBytes);

// Prepend a "[elapsed=<n>ms]" header when `elapsed_ms >= threshold_ms`,
// otherwise return `content` unchanged. Pure so we can unit-test.
std::string annotate_with_elapsed(const std::string& content,
                                  double elapsed_ms,
                                  double threshold_ms = 250.0);

// Wrap a tool result in a fenced block with a system note when it is
// part of a subagent delegation's output. Matches the Python
// `<delegated-subagent-result>` fencing used by delegate_tool.
std::string build_subagent_result_block(const std::string& child_task,
                                        const std::string& child_output);

}  // namespace hermes::agent::tool_dispatch
