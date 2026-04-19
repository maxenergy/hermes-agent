// Strip reasoning/thinking tag blocks from assistant content.
//
// C++17 port of run_agent.py::AIAgent._strip_think_blocks.
// Upstream commits: 9489d157 (unterminated handling) and ec48ec55
// (stored assistant content).
//
// Handles four cases:
//   1. Closed tag pairs (``<think>…</think>``) — the common path when
//      the provider emits complete reasoning blocks.
//   2. Unterminated open tag at a block boundary (start of text or
//      after a newline) — e.g. MiniMax M2.7 / NIM endpoints where the
//      closing tag is dropped. Everything from the open tag to end of
//      string is stripped. The block-boundary check avoids over-stripping
//      prose that mentions ``<think>`` mid-sentence.
//   3. Stray orphan open/close tags that slip through.
//   4. Tag variants: ``<think>``, ``<thinking>``, ``<reasoning>``,
//      ``<REASONING_SCRATCHPAD>``, ``<thought>`` (Gemma 4), all
//      case-insensitive.
#pragma once

#include <string>

namespace hermes::agent {

// Remove reasoning/thinking blocks from ``content`` and return the
// visible remainder. Input is not modified; an empty input yields an
// empty string.
std::string strip_think_blocks(const std::string& content);

}  // namespace hermes::agent
