// Skills Hub client. All methods return empty / false until the GitHub
// App JWT auth + HTTP fetch backend is connected.
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
    // Returns empty vector when hub backend is not connected.
    std::vector<HubSkillEntry> search(const std::string& query);

    // Returns std::nullopt when hub backend is not connected.
    std::optional<HubSkillEntry> get(const std::string& name);

    // Returns false when hub backend is not connected.
    bool install(const std::string& name);

    // Returns false when hub backend is not connected.
    bool uninstall(const std::string& name);

    // Returns false when hub backend is not connected.
    bool update(const std::string& name);

private:
    // GitHub App JWT auth + HTTP fetch backend (not yet connected).
};

}  // namespace hermes::skills
