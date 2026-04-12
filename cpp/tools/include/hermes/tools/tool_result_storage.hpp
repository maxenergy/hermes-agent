// Spillover storage for oversized tool results.
//
// When a tool emits a payload larger than the per-tool truncation limit,
// the AIAgent (Phase 4) writes the full payload via this store and replaces
// the inline content with a short ``hermes://tool-result/<uuid>`` handle.
// Phase 8 file/read tools then resolve the handle.
//
// Phase 5 only delivers the storage primitive; the persistence layer that
// actually invokes it lives in Phase 4.
#pragma once

#include <chrono>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace hermes::tools {

class ToolResultStorage {
public:
    explicit ToolResultStorage(std::filesystem::path dir);

    // Persist ``content`` and return a handle of the form
    // ``hermes://tool-result/<uuid>``.  Throws std::runtime_error on
    // filesystem errors (cannot create dir, write fails, ...).
    std::string store(std::string_view content);

    // Resolve a previously-issued handle.  Returns std::nullopt when the
    // handle is unknown / malformed / the file has been removed.
    std::optional<std::string> retrieve(std::string_view handle);

    // Remove on-disk entries older than ``age``.  Uses
    // ``std::filesystem::last_write_time``.  Returns the number of files
    // that were removed.
    std::size_t cleanup_older_than(std::chrono::seconds age);

    const std::filesystem::path& directory() const { return dir_; }

private:
    std::filesystem::path dir_;
};

// Handle prefix used by ToolResultStorage::store().
constexpr const char* TOOL_RESULT_HANDLE_PREFIX = "hermes://tool-result/";

}  // namespace hermes::tools
