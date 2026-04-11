// MemoryStore — two-file curated memory (MEMORY.md and USER.md) where
// entries are separated by the § (U+00A7) section sign. File mutations
// acquire a POSIX advisory lock via fcntl(F_SETLK) on a sibling .lock
// file, so concurrent writers in the same (or different) processes do
// not clobber each other.
#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace hermes::state {

enum class MemoryFile { Agent, User };  // MEMORY.md vs USER.md

class MemoryStore {
public:
    // Uses get_hermes_home() / "memories" for the directory.
    MemoryStore();
    explicit MemoryStore(const std::filesystem::path& memories_dir);

    // Return every entry in the requested file. Empty entries are
    // discarded. The file is created on first access if needed.
    std::vector<std::string> read_all(MemoryFile which);

    // Append a new entry. The entry is trimmed of leading/trailing
    // whitespace and an empty entry is a no-op.
    void add(MemoryFile which, std::string_view entry);

    // Replace the first entry that contains `needle` (substring match)
    // with `replacement`. No-op if nothing matches.
    void replace(MemoryFile which,
                 std::string_view needle,
                 std::string_view replacement);

    // Delete the first entry that contains `needle`.
    void remove(MemoryFile which, std::string_view needle);

    struct ThreatHit {
        std::string pattern;
        std::string matched_text;
    };
    // Run the builtin threat-pattern scanner over `content`. The caller
    // decides whether to block on non-empty results — MemoryStore never
    // refuses to write.
    std::vector<ThreatHit> scan_for_threats(std::string_view content);

    // Expose the resolved on-disk path for diagnostics / tests.
    std::filesystem::path path_for(MemoryFile which) const;

private:
    std::filesystem::path dir_;
};

}  // namespace hermes::state
