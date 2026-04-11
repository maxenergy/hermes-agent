// Atomic write helpers — write to a sibling tempfile, fsync, rename
// into place. Never throws; all failures surface via `false` / empty.
#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace hermes::core::atomic_io {

namespace fs = std::filesystem;

// Write `content` to `dst` atomically. On success the final file
// contains exactly `content`. On failure any temporary file is removed
// and `false` is returned.
bool atomic_write(const fs::path& dst, std::string_view content) noexcept;

// Read `src` into a string. Returns std::nullopt when the file does not
// exist or cannot be read.
std::optional<std::string> atomic_read(const fs::path& src) noexcept;

}  // namespace hermes::core::atomic_io
