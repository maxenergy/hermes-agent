#include "hermes/skills/skills_hub.hpp"

namespace hermes::skills {

std::vector<HubSkillEntry> SkillsHub::search(const std::string& /*query*/) {
    // TODO(phase-13): implement GitHub App JWT auth + HTTP fetch
    return {};
}

std::optional<HubSkillEntry> SkillsHub::get(const std::string& /*name*/) {
    // TODO(phase-13): implement
    return std::nullopt;
}

bool SkillsHub::install(const std::string& /*name*/) {
    // TODO(phase-13): implement
    return false;
}

bool SkillsHub::uninstall(const std::string& /*name*/) {
    // TODO(phase-13): implement
    return false;
}

bool SkillsHub::update(const std::string& /*name*/) {
    // TODO(phase-13): implement
    return false;
}

}  // namespace hermes::skills
