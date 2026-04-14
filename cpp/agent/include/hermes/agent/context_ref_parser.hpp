// @-reference parser (port of agent/context_references.py parser half).
//
// Parses @file:..., @folder:..., @url:..., @git:..., @diff, @staged
// tokens out of a user message and exposes the structured references
// for downstream expansion. This module focuses on the parsing side;
// expansion (reading files, running git) lives in the caller so we
// don't pull subprocess / network code into the agent library.
#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace hermes::agent {

struct ParsedContextRef {
    std::string raw;      // the exact @… substring, including the prefix
    std::string kind;     // file | folder | git | url | diff | staged
    std::string target;   // resolved target (unquoted, sans :line range)
    std::size_t start = 0;
    std::size_t end = 0;
    std::optional<int> line_start;
    std::optional<int> line_end;
};

// Parse all @-references out of `message`. Returns them in source-order.
std::vector<ParsedContextRef> parse_context_references(
    const std::string& message);

// Remove the @-reference tokens from the original message and collapse
// redundant whitespace (single spaces, no orphaned ", ." before punct).
std::string remove_reference_tokens(
    const std::string& message,
    const std::vector<ParsedContextRef>& refs);

// Strip common trailing punctuation that accidentally got caught by
// the regex (", ." / ") ] }" when unbalanced, etc.). Exposed for tests.
std::string strip_trailing_punctuation(const std::string& value);

// Strip matching outer quotes (backtick / ' / ") from a reference value.
std::string strip_reference_wrappers(const std::string& value);

// Parse a file: reference value into (path, line_start, line_end).
struct FileRefValue {
    std::string path;
    std::optional<int> line_start;
    std::optional<int> line_end;
};
FileRefValue parse_file_reference_value(const std::string& value);

// Return a code-fence language hint for a path suffix (e.g. "py" -> "python").
std::string code_fence_language(const std::string& path);

}  // namespace hermes::agent
