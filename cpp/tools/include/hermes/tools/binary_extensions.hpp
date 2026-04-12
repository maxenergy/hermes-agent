// Binary file extension table — pure string check, no I/O.
//
// Used by file-text tools (read/search/grep) to skip files that can't be
// meaningfully read as text.  Ported from tools/binary_extensions.py with
// the addition of an in-memory content sniffer (null-byte heuristic).
#pragma once

#include <filesystem>
#include <string_view>
#include <unordered_set>

namespace hermes::tools {

// Check by extension.  Comparison is case-insensitive on the extension
// only — the rest of the path is ignored.
bool is_binary_extension(const std::filesystem::path& path);

// Inspect a chunk read from disk and return true when it looks binary.
// Heuristic: any NUL byte in the first chunk is treated as binary.
bool is_likely_binary_content(std::string_view first_chunk);

// Read-only access to the full extension set (lower-case, dot-prefixed).
const std::unordered_set<std::string>& binary_extensions();

}  // namespace hermes::tools
