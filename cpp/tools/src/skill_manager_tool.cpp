#include "hermes/tools/skill_manager_tool.hpp"

#include "hermes/core/path.hpp"
#include "hermes/tools/registry.hpp"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace hermes::tools {

using json = nlohmann::json;
namespace fs = std::filesystem;

namespace {

fs::path skills_root() {
    return hermes::core::path::get_hermes_home() / "skills";
}

std::string read_file_contents(const fs::path& p) {
    std::ifstream ifs(p);
    if (!ifs.is_open()) return {};
    std::ostringstream ss;
    ss << ifs.rdbuf();
    return ss.str();
}

}  // namespace

void register_skill_manager_tools(ToolRegistry& registry) {
    ToolEntry e;
    e.name = "skill_manage";
    e.toolset = "skills";
    e.description = "Manage skills — list, search, install, uninstall, update";
    e.emoji = "\xf0\x9f\x94\xa7";  // wrench

    e.schema = json::parse(R"JSON({
        "type": "object",
        "properties": {
            "action": {
                "type": "string",
                "enum": ["list_available", "list_installed", "search",
                         "install", "uninstall", "update"],
                "description": "Action to perform"
            },
            "query": {
                "type": "string",
                "description": "Search query (for search action)"
            },
            "name": {
                "type": "string",
                "description": "Skill name (for install/uninstall/update)"
            }
        },
        "required": ["action"]
    })JSON");

    e.handler = [](const json& args, const ToolContext& /*ctx*/) -> std::string {
        if (!args.contains("action") || !args["action"].is_string()) {
            return tool_error("missing required parameter: action");
        }
        auto action = args["action"].get<std::string>();

        if (action == "list_installed") {
            json skills = json::array();
            auto dir = skills_root();
            std::error_code ec;
            if (fs::is_directory(dir, ec)) {
                for (const auto& entry : fs::directory_iterator(dir, ec)) {
                    if (!entry.is_directory()) continue;
                    auto idx = entry.path() / "index.json";
                    auto md = entry.path() / "SKILL.md";
                    if (!fs::exists(idx, ec) && !fs::exists(md, ec)) continue;

                    std::string name = entry.path().filename().string();
                    std::string description;
                    if (fs::exists(idx, ec)) {
                        auto content = read_file_contents(idx);
                        if (!content.empty()) {
                            try {
                                auto j = json::parse(content);
                                description = j.value("description", "");
                            } catch (...) {}
                        }
                    }
                    skills.push_back({{"name", name},
                                      {"description", description}});
                }
            }
            return tool_result({{"skills", skills},
                                {"count", static_cast<int>(skills.size())}});
        }

        if (action == "list_available") {
            return tool_error("Skills Hub not connected");
        }

        if (action == "search") {
            return tool_error("Skills Hub not connected");
        }

        if (action == "install") {
            return tool_error("Skills Hub not connected — cannot install skills");
        }

        if (action == "uninstall") {
            auto name = args.value("name", "");
            if (name.empty()) {
                return tool_error("missing required parameter: name");
            }
            // Safety check: name must not contain path separators.
            if (name.find('/') != std::string::npos ||
                name.find('\\') != std::string::npos ||
                name == ".." || name == ".") {
                return tool_error("invalid skill name: " + name);
            }
            auto skill_dir = skills_root() / name;
            std::error_code ec;
            // Safety: the skill dir must be under the skills root.
            auto canonical_root = fs::weakly_canonical(skills_root(), ec);
            auto canonical_skill = fs::weakly_canonical(skill_dir, ec);
            auto root_str = canonical_root.string();
            auto skill_str = canonical_skill.string();
            if (skill_str.substr(0, root_str.size()) != root_str) {
                return tool_error("skill path escapes skills directory");
            }
            if (!fs::is_directory(skill_dir, ec)) {
                return tool_error("skill not found: " + name);
            }
            fs::remove_all(skill_dir, ec);
            if (ec) {
                return tool_error("failed to remove skill: " + ec.message());
            }
            return tool_result({{"uninstalled", true}, {"name", name}});
        }

        if (action == "update") {
            return tool_error("Skills Hub not connected — cannot update skills");
        }

        return tool_error("unknown action: " + action);
    };

    registry.register_tool(std::move(e));
}

}  // namespace hermes::tools
