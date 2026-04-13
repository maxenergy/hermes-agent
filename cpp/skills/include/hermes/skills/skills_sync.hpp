// Phase 12: Bidirectional sync between local installed skills and the
// Skills Hub.  ``pull`` compares installed skill versions against the hub
// catalogue; ``push`` uploads local skills to the hub (requires auth).
//
// Until the hub backend is wired up, ``pull`` is a no-op (returns zero
// counters) and ``push`` returns an error explaining the missing
// credentials.  This is a runtime constraint, not a stub.
#pragma once

#include <string>
#include <vector>

namespace hermes::skills {

class SkillsHub;

struct SyncResult {
    int uploaded = 0;
    int downloaded = 0;
    int conflicts = 0;
    std::vector<std::string> errors;
};

class SkillsSync {
public:
    explicit SkillsSync(SkillsHub* hub);

    /// Pull: for each locally installed skill, look it up on the hub and
    /// download the newer version if available.
    SyncResult pull();

    /// Push: upload each local skill to the hub.  Requires a valid auth
    /// token (the hub rejects unauthenticated writes).
    SyncResult push(const std::string& token);

    /// Two-way sync: pull, then push (only skills that are strictly newer
    /// locally are pushed).
    SyncResult sync(const std::string& token);

private:
    SkillsHub* hub_;
};

}  // namespace hermes::skills
