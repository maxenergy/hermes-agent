// Minimal string utilities modelled after the helpers used throughout the
// Python codebase. Pure C++17, no ICU, no locale-aware transforms.
#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace hermes::core::strings {

// Split `input` by the delimiter `delim`. Empty segments are kept.
// An empty input yields a single empty element — matching Python's
// `"".split("x")` behaviour.
std::vector<std::string> split(std::string_view input, std::string_view delim);

// Join a range of strings with `sep` between them.
std::string join(const std::vector<std::string>& parts, std::string_view sep);

// True when `input` starts with `prefix`.
bool starts_with(std::string_view input, std::string_view prefix) noexcept;

// True when `input` ends with `suffix`.
bool ends_with(std::string_view input, std::string_view suffix) noexcept;

// Return `input` with leading and trailing ASCII whitespace removed.
std::string trim(std::string_view input);

// ASCII-only lower/upper transforms — does not touch UTF-8 multi-byte runes.
std::string to_lower(std::string_view input);
std::string to_upper(std::string_view input);

// True when `needle` occurs anywhere in `haystack`.
bool contains(std::string_view haystack, std::string_view needle) noexcept;

// Replace any lone UTF-16 surrogate code point (U+D800..U+DFFF) encoded
// as UTF-8 (3-byte sequence ED A0..BF 80..BF) with U+FFFD (EF BF BD).
// Surrogates are invalid in UTF-8 and will crash ``json.dumps()`` inside
// third-party SDKs; some models (Kimi, GLM, Qwen via Ollama) emit them
// in reasoning / content and the bad bytes flow back through the next
// turn's ``api_messages``.
//
// Ports upstream Python commit 8798b069 (_sanitize_surrogates).
//
// Fast no-op when `input` contains no surrogates — the byte-pattern scan
// only touches input when an ED byte is seen.  Returns a new string; the
// input is not modified.
std::string sanitize_surrogates(std::string_view input);

// True iff `input` contains at least one UTF-8-encoded lone surrogate.
// Useful for branch-predicate paths that want to avoid an unconditional
// copy when the common case is surrogate-free.
bool contains_surrogate(std::string_view input) noexcept;

}  // namespace hermes::core::strings
