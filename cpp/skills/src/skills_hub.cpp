#include "hermes/skills/skills_hub.hpp"

#include "hermes/llm/llm_client.hpp"

#include <nlohmann/json.hpp>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

namespace hermes::skills {

namespace {

const std::string kHubBaseUrl = "https://agentskills.io/api/skills";

std::string skills_dir() {
    const char* home = std::getenv("HOME");
    if (!home) return "";
    return std::string(home) + "/.hermes/skills";
}

}  // namespace

std::vector<HubSkillEntry> SkillsHub::search(const std::string& query) {
    auto* transport = hermes::llm::get_default_transport();
    if (!transport) return {};

    std::unordered_map<std::string, std::string> headers;
    headers["Accept"] = "application/json";

    auto resp = transport->get(kHubBaseUrl + "?q=" + query, headers);
    if (resp.status_code != 200) return {};

    auto body = nlohmann::json::parse(resp.body, nullptr, false);
    if (body.is_discarded() || !body.is_array()) return {};

    std::vector<HubSkillEntry> results;
    for (const auto& item : body) {
        HubSkillEntry entry;
        entry.name = item.value("name", "");
        entry.description = item.value("description", "");
        entry.version = item.value("version", "");
        entry.author = item.value("author", "");
        entry.repo_url = item.value("repo_url", "");
        results.push_back(std::move(entry));
    }
    return results;
}

std::optional<HubSkillEntry> SkillsHub::get(const std::string& name) {
    auto* transport = hermes::llm::get_default_transport();
    if (!transport) return std::nullopt;

    std::unordered_map<std::string, std::string> headers;
    headers["Accept"] = "application/json";

    auto resp = transport->get(kHubBaseUrl + "/" + name, headers);
    if (resp.status_code != 200) return std::nullopt;

    auto body = nlohmann::json::parse(resp.body, nullptr, false);
    if (body.is_discarded()) return std::nullopt;

    HubSkillEntry entry;
    entry.name = body.value("name", "");
    entry.description = body.value("description", "");
    entry.version = body.value("version", "");
    entry.author = body.value("author", "");
    entry.repo_url = body.value("repo_url", "");
    return entry;
}

bool SkillsHub::install(const std::string& name) {
    auto* transport = hermes::llm::get_default_transport();
    if (!transport) return false;

    // Fetch skill metadata.
    std::unordered_map<std::string, std::string> headers;
    headers["Accept"] = "application/json";

    auto resp = transport->get(kHubBaseUrl + "/" + name, headers);
    if (resp.status_code != 200) return false;

    auto body = nlohmann::json::parse(resp.body, nullptr, false);
    if (body.is_discarded()) return false;

    auto base = skills_dir();
    if (base.empty()) return false;

    auto skill_path = std::filesystem::path(base) / name;
    std::filesystem::create_directories(skill_path);

    // Download SKILL.md if available.
    if (body.contains("skill_md_url")) {
        auto md_url = body["skill_md_url"].get<std::string>();
        auto md_resp = transport->get(md_url, {});
        if (md_resp.status_code == 200) {
            std::ofstream ofs(skill_path / "SKILL.md");
            ofs << md_resp.body;
        }
    }

    // Download additional files if listed.
    if (body.contains("files") && body["files"].is_array()) {
        for (const auto& file_entry : body["files"]) {
            auto file_url = file_entry.value("url", "");
            auto file_name = file_entry.value("name", "");
            if (file_url.empty() || file_name.empty()) continue;
            auto file_resp = transport->get(file_url, {});
            if (file_resp.status_code == 200) {
                std::ofstream ofs(skill_path / file_name);
                ofs << file_resp.body;
            }
        }
    }

    // Write metadata.
    {
        std::ofstream ofs(skill_path / "metadata.json");
        ofs << body.dump(2);
    }

    return true;
}

bool SkillsHub::uninstall(const std::string& name) {
    auto base = skills_dir();
    if (base.empty()) return false;

    auto skill_path = std::filesystem::path(base) / name;

    // Safety: ensure the path is actually under the skills root.
    auto canonical_base = std::filesystem::weakly_canonical(base);
    auto canonical_skill = std::filesystem::weakly_canonical(skill_path);
    auto base_str = canonical_base.string();
    auto skill_str = canonical_skill.string();
    if (skill_str.find(base_str) != 0) return false;

    if (!std::filesystem::exists(skill_path)) return false;
    std::filesystem::remove_all(skill_path);
    return true;
}

bool SkillsHub::update(const std::string& name) {
    if (!uninstall(name)) {
        // If not installed, just try a fresh install.
    }
    return install(name);
}

}  // namespace hermes::skills
