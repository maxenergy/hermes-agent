// Tool result envelope helpers — truncation + standardization.
//
// Tools always return a JSON-encoded string.  These helpers normalize the
// shape (so the LLM sees a consistent envelope) and apply size truncation
// when a result exceeds the configured per-tool budget.
#pragma once

#include <nlohmann/json.hpp>

#include <cstddef>
#include <string>
#include <string_view>

namespace hermes::tools {

// Truncate a (UTF-8) JSON-encoded result string to ``max_size`` bytes.
//
// If ``json_result`` already fits, it is returned unchanged.  Otherwise the
// trailing bytes are replaced with a marker of the form
// `` [...truncated N chars...]`` where N is the number of bytes that were
// removed.  The total length of the returned string is guaranteed to be
// ``<= max_size`` whenever ``max_size`` is large enough to hold the marker;
// for absurdly small ``max_size`` values the marker itself is still emitted.
std::string truncate_result(std::string_view json_result, std::size_t max_size);

// Standardize an arbitrary JSON value into a tool-result envelope.
//
//   * If ``raw`` is a JSON object, it is returned as-is (the keys are
//     considered the public envelope).
//   * Otherwise the value is wrapped in ``{"output": raw}`` so handlers
//     that yield arrays / strings / numbers always produce an object at
//     the top level.
nlohmann::json standardize(const nlohmann::json& raw);

}  // namespace hermes::tools
