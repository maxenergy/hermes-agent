// Memory tool — CRUD operations on the hermes MemoryStore (MEMORY.md / USER.md).
//
// Mirrors ``tools/memory_tool.py`` — the heavy work (file locking, snapshot
// rendering) lives in hermes::state::MemoryStore.  This module is responsible
// for argument parsing, threat scanning, char-limit accounting, and emitting
// the canonical JSON envelope expected by the Python client.
//
// Helpers exposed below are unit-tested in isolation so the dispatch shim
// itself stays declarative.
#pragma once

#include "hermes/state/memory_store.hpp"
#include "hermes/tools/registry.hpp"

#include <nlohmann/json.hpp>

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace hermes::tools {

// Char-budget defaults — match the Python implementation.
constexpr std::size_t kMemoryAgentCharLimit = 2200;
constexpr std::size_t kMemoryUserCharLimit = 1375;

// Section-sign delimiter used between entries in MEMORY.md / USER.md.
extern const char* const kMemoryEntryDelimiter;  // "\n§\n"

void register_memory_tools(ToolRegistry& registry);

// ---- Helpers exposed for tests -------------------------------------------

// Translate the JSON ``file`` argument ("agent" or "user") to the typed
// MemoryFile enum.  Defaults to Agent for unknown values.
hermes::state::MemoryFile parse_memory_file(const nlohmann::json& args);

// Per-target char limit ceiling.
std::size_t char_limit_for(hermes::state::MemoryFile which);

// The single-string serialisation hermes uses on disk: entries joined by
// the section-sign delimiter.  Used to compute char budgets without
// touching disk.
std::string join_entries(const std::vector<std::string>& entries);

// Strip leading/trailing ASCII whitespace + collapse internal runs of
// the section sign that would otherwise corrupt the on-disk format.
std::string sanitize_entry(std::string_view raw);

// Returns true if ``content`` contains any of the known invisible
// unicode markers used in injection attacks.
bool contains_invisible_unicode(std::string_view content);

// Scan ``content`` for prompt-injection / exfiltration markers.  When a
// hit is found, returns the matching threat id; otherwise nullopt.
std::optional<std::string> scan_memory_content(std::string_view content);

// Format the canonical "usage" string ("1,234/2,200" with thousands
// separators) used in error envelopes.
std::string format_usage(std::size_t used, std::size_t limit);

// Find the indexes of every entry that contains ``needle`` as a
// substring.  Order preserved.
std::vector<std::size_t> find_matching_indexes(
    const std::vector<std::string>& entries, std::string_view needle);

// Build the ``add`` envelope for a successful insert / dedup.  Centralised
// so the schema stays in sync.
nlohmann::json build_add_response(hermes::state::MemoryFile which,
                                  const std::vector<std::string>& entries,
                                  std::string_view note);

}  // namespace hermes::tools
