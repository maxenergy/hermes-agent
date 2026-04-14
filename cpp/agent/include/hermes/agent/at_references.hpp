// At-reference parsing (@file:… / @folder:… / @git / @diff / @staged / @url).
//
// Partial C++17 port of agent/context_references.py's @-reference model.
// Provides the pure lexing + path resolution + sensitive-path guard
// portions, along with a synchronous expander for file references.
// URL fetching and subprocess-based git expansion are left as caller
// responsibilities (the caller supplies fetcher / git callbacks).
#pragma once

#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace hermes::agent::atref {

enum class RefKind {
    File,
    Folder,
    Git,
    Diff,
    Staged,
    Url,
    Unknown,
};

std::string kind_name(RefKind k);

struct AtReference {
    std::string raw;          // full matched token e.g. "@file:main.py:10-20"
    RefKind kind = RefKind::Unknown;
    std::string target;       // normalised path / URL / count
    std::size_t start = 0;    // byte offset in source message
    std::size_t end = 0;
    std::optional<int> line_start;
    std::optional<int> line_end;
};

// Parse all @ references from a message. The scanner mirrors the
// Python regex: a reference must follow a non-[word, '/'] character
// (or start-of-string) and is one of the simple verbs "@diff" / "@staged"
// or the kind:value form "@file:path" / "@folder:path" / "@git:count"
// / "@url:url". File paths may be backtick / single / double quoted and
// may carry a ":<start>[-<end>]" line range.
std::vector<AtReference> parse_at_references(const std::string& message);

// Remove all matched @ tokens from `message`, returning the cleaned-up
// text. Collapses double spaces and fixes orphaned punctuation, mirroring
// Python's _remove_reference_tokens.
std::string remove_reference_tokens(const std::string& message,
                                    const std::vector<AtReference>& refs);

// Resolve a target path relative to cwd, expanding ~ and blocking
// traversal outside `allowed_root` (default: cwd). Throws std::runtime_error
// if the path escapes `allowed_root`.
std::filesystem::path resolve_reference_path(
    const std::filesystem::path& cwd,
    const std::string& target,
    const std::filesystem::path& allowed_root = {});

// Raise std::runtime_error if `path` points at a sensitive credential
// file or directory (~/.ssh, ~/.aws, ~/.hermes/.env, ...).
void check_reference_path_allowed(
    const std::filesystem::path& path,
    const std::filesystem::path& home,
    const std::filesystem::path& hermes_home);

// Helpers exposed for tests.
namespace detail {

std::string strip_trailing_punctuation(std::string value);
std::string strip_reference_wrappers(std::string value);
// Parse "path", "path:10", "path:10-20", or quoted "foo bar":10-20.
// Returns {path, start, end}. start/end are std::nullopt when absent.
void parse_file_reference_value(const std::string& value,
                                std::string& out_path,
                                std::optional<int>& out_start,
                                std::optional<int>& out_end);

}  // namespace detail

}  // namespace hermes::agent::atref
