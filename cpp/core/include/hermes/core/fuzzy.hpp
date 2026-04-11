// Levenshtein distance + fuzzy-contains helper.
#pragma once

#include <string_view>

namespace hermes::core::fuzzy {

// Classic DP Levenshtein distance. Uses O(|a|*|b|) space — perfectly
// adequate for the short-string inputs we see in Phase 0 (tool names,
// model aliases, CLI commands).
int levenshtein(std::string_view a, std::string_view b);

// True when `needle` occurs inside `haystack` with at most `max_distance`
// edits from any contiguous substring of `haystack` of length
// `|needle|`. Empty needle always matches.
bool fuzzy_contains(std::string_view haystack, std::string_view needle, int max_distance = 2);

}  // namespace hermes::core::fuzzy
