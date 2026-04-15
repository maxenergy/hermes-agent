// Depth port of environments/agentic_opd_env.py pure helpers.
//
// The Atropos OPD (on-policy distillation) environment exposes a
// handful of message-shape utilities that do not depend on the
// async Atropos runtime: hint-judge prompt assembly, boxed-score /
// hint parsing, majority-vote selection, hint injection into the last
// user message, turn-pair extraction from OpenAI-style message
// histories, and a token-span search over integer ID lists.  These all
// come out to small regex / list manipulations that port cleanly to
// C++17 and let us assert the behavioural parity of the Atropos env
// without running a Python interpreter.
#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

namespace hermes::environments::opd_helpers {

// --- Hint-judge messages ------------------------------------------------

// Default judge system prompt (mirrors ``_HINT_JUDGE_SYSTEM``).  Exposed
// as a function so callers can override in tests without rebuilding.
std::string default_hint_judge_system();

// Build the two-message prompt for the hint judge.  Mirrors
// ``_build_hint_judge_messages`` exactly.
std::vector<nlohmann::json>
build_hint_judge_messages(std::string_view response_text,
                            std::string_view next_state_text,
                            std::string_view next_state_role = "tool",
                            std::string_view judge_system = {});

// --- Judge result parsing ----------------------------------------------

struct HintResult {
    std::optional<int> score;  // +1 / -1, or unset
    std::string hint;
};

// Parse the judge output: the last ``\boxed{...}`` carries the score
// (only ``1`` / ``-1`` are kept) and the last ``[HINT_START]...[HINT_END]``
// span carries the hint text (stripped).
HintResult parse_hint_result(std::string_view text);

// Select the best hint from a vector of majority-vote results.  Keeps
// only positive votes with hint text longer than ten non-whitespace
// characters (after strip), then returns the longest.  Returns
// ``std::nullopt`` when no vote qualifies.
std::optional<HintResult>
select_best_hint(const std::vector<HintResult>& votes);

// --- Hint injection -----------------------------------------------------

// Append ``hint`` to the last user message in a deep-copied message
// list.  When there is no user message, creates one with the hint
// prefixed by the canonical ``[user's hint / instruction]`` marker.
// Message content that is a list of content-parts is flattened to text
// before the suffix is added, matching the Python helper.
std::vector<nlohmann::json>
append_hint_to_messages(const std::vector<nlohmann::json>& messages,
                          std::string_view hint);

// --- Turn pairs ---------------------------------------------------------

struct TurnPair {
    std::vector<nlohmann::json> context_messages;
    std::string assistant_text;
    std::string next_state_text;
    std::string next_state_role;  // "tool" | "user"
};

// Walk a message list and extract (assistant, next_state) pairs.  The
// next state is the concatenation (joined by ``\n---\n``) of all
// contiguous tool-role messages following the assistant turn, plus the
// first user message if it comes before another assistant turn.  Tool
// outputs longer than ``max_next_state_chars`` are truncated with a
// trailing ``\n...[truncated]`` sentinel to match Python's behaviour.
std::vector<TurnPair>
extract_turn_pairs(const std::vector<nlohmann::json>& messages,
                    std::size_t max_next_state_chars = 4000);

// --- Token-span search --------------------------------------------------

// Find where ``sub`` appears inside ``full``.  Returns the start index
// of the last match (Python searches from the end because assistant
// responses typically sit at the tail of the sequence).  Returns
// ``std::nullopt`` when not found or when either input is empty.
std::optional<std::size_t>
find_token_span(const std::vector<int>& full, const std::vector<int>& sub);

}  // namespace hermes::environments::opd_helpers
