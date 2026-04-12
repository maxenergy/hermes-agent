// Skills Hub client — stub for Phase 9.  All methods return empty / false
// until Phase 13 implements GitHub App JWT auth + HTTP fetch.
#pragma once

#include <optional>
#include <string>
#include <vector>

namespace hermes::skills {

struct HubSkillEntry {
    std::string name;
    std::string description;
    std::string version;
    std::string author;
    std::string repo_url;
};

class SkillsHub {
public:
    // Phase 9: stub.  Returns empty vector.
    std::vector<HubSkillEntry> search(const std::string& query);

    // Phase 9: stub.  Returns std::nullopt.
    std::optional<HubSkillEntry> get(const std::string& name);

    // Phase 9: stub.  Returns false.
    bool install(const std::string& name);

    // Phase 9: stub.  Returns false.
    bool uninstall(const std::string& name);

    // Phase 9: stub.  Returns false.
    bool update(const std::string& name);

private:
    // TODO(phase-13): GitHub App JWT auth + HTTP fetch
};

}  // namespace hermes::skills
