// Budget constants for the tool result persistence system.
//
// Ported from tools/budget_config.py.  These values control the three-layer
// defense against context-window overflow:
//   1. per-tool cap (authored inside each tool)
//   2. per-result persistence threshold (DEFAULT_RESULT_SIZE_CHARS)
//   3. per-turn aggregate budget (DEFAULT_TURN_BUDGET_CHARS)
//
// DEFAULT_RESULT_SIZE_CHARS keeps the spec-level 32 KiB guarantee for
// dispatch-time truncation.  Python's value is 100_000 chars, but the
// spec here pins the in-memory truncation ceiling lower.  Both are
// exported so callers can pick the appropriate one.
#pragma once

#include <cstddef>

namespace hermes::tools {

// Dispatch-time truncation ceiling used by ToolRegistry::dispatch when a
// ToolEntry does not set its own max_result_size_chars.  The spec pins this
// at 32 KiB per tool call.
constexpr std::size_t DEFAULT_RESULT_SIZE_CHARS = 32 * 1024;

// Python-parity persistence threshold (100K chars).  Exposed for callers
// that mirror the Python budget layer rather than the C++ spec default.
constexpr std::size_t DEFAULT_PERSIST_RESULT_SIZE_CHARS = 100000;

// Aggregate per-turn budget across all tool results in a single assistant
// turn (200 KiB in Python).
constexpr std::size_t DEFAULT_TURN_BUDGET_CHARS = 200000;

// Preview size kept inline when a large result is spilled to disk.
constexpr std::size_t DEFAULT_PREVIEW_SIZE_CHARS = 1500;

// Tool-call ceiling hints used by the AIAgent when enforcing limits
// on a single model turn / full session.
constexpr int DEFAULT_MAX_TOOL_CALLS_PER_TURN = 20;
constexpr int DEFAULT_MAX_TOTAL_TOOL_CALLS = 500;

}  // namespace hermes::tools
