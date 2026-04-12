// File operation helpers — path validation, binary detection, content reading.
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

namespace hermes::tools {

namespace fs = std::filesystem;

// Returns true when the path is a sensitive system file that should not be
// written to by the agent (e.g. /etc/passwd, ~/.ssh/authorized_keys).
bool is_sensitive_path(const fs::path& p);

// Returns true when the file is likely binary — checks extension first,
// then (if needed) inspects the first 8 KB of content.
bool is_binary_file(const fs::path& p);

// Read file content starting at byte offset `offset`.  When `limit` is -1
// the entire remainder is returned.  Throws std::runtime_error on I/O
// failure.
std::string read_file_content(const fs::path& p, int64_t offset = 0,
                              int64_t limit = -1);

}  // namespace hermes::tools
