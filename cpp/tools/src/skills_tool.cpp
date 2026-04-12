#include "hermes/tools/skills_tool.hpp"

#include "hermes/core/path.hpp"
#include "hermes/tools/registry.hpp"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace hermes::tools {

namespace {

namespace fs = std::filesystem;

fs::path skills_dir() {
    return hermes::core::path::get_hermes_home() / "skills";
}

// Read a file's full contents as a string. Returns empty on error.
std::string read_file(const fs::path& p) {
    std::ifstream ifs(p);
    if (!ifs.is_open()) return {};
    std::ostringstream ss;
    ss << ifs.rdbuf();
    return ss.str();
}

}  // namespace

void register_skills_tools(ToolRegistry& registry) {
    // ----- skills_list -----
    {
        ToolEntry e;
        e.name = "skills_list";
        e.toolset = "skills";
        e.description = "List installed skills";
        e.emoji = "\xf0\x9f\x93\x9a";  // books
        e.schema = nlohmann::json::parse(R"JSON({
            "type": "object",
            "properties": {}
        })JSON");

        e.handler = [](const nlohmann::json& /*args*/,
                       const ToolContext& /*ctx*/) -> std::string {
            nlohmann::json skills = nlohmann::json::array();
            const auto dir = skills_dir();
            std::error_code ec;
            if (!fs::is_directory(dir, ec)) {
                return tool_result(
                    {{"skills", skills}, {"count", 0}});
            }
            for (const auto& entry : fs::directory_iterator(dir, ec)) {
                if (!entry.is_directory()) continue;
                auto idx = entry.path() / "index.json";
                auto md = entry.path() / "SKILL.md";
                if (!fs::exists(idx, ec) && !fs::exists(md, ec)) continue;

                std::string name = entry.path().filename().string();
                std::string description;

                // Try to get description from index.json
                if (fs::exists(idx, ec)) {
                    auto content = read_file(idx);
                    if (!content.empty()) {
                        try {
                            auto j = nlohmann::json::parse(content);
                            description = j.value("description", "");
                        } catch (...) {
                            // ignore parse errors
                        }
                    }
                }

                skills.push_back(
                    {{"name", name}, {"description", description}});
            }
            return tool_result(
                {{"skills", skills},
                 {"count", static_cast<int>(skills.size())}});
        };

        registry.register_tool(std::move(e));
    }

    // ----- skill_view -----
    {
        ToolEntry e;
        e.name = "skill_view";
        e.toolset = "skills";
        e.description = "View a skill's content (SKILL.md or sub-file)";
        e.emoji = "\xf0\x9f\x93\x96";  // open book
        e.schema = nlohmann::json::parse(R"JSON({
            "type": "object",
            "properties": {
                "name": {
                    "type": "string",
                    "description": "Skill name (directory name)"
                },
                "file": {
                    "type": "string",
                    "description": "Optional sub-file to view (default: SKILL.md)"
                }
            },
            "required": ["name"]
        })JSON");

        e.handler = [](const nlohmann::json& args,
                       const ToolContext& /*ctx*/) -> std::string {
            if (!args.contains("name") || !args["name"].is_string()) {
                return tool_error("missing required parameter: name");
            }
            auto name = args["name"].get<std::string>();
            auto dir = skills_dir() / name;

            std::error_code ec;
            if (!fs::is_directory(dir, ec)) {
                return tool_error("skill not found: " + name);
            }

            std::string filename = "SKILL.md";
            if (args.contains("file") && args["file"].is_string()) {
                filename = args["file"].get<std::string>();
            }

            auto filepath = dir / filename;
            if (!fs::exists(filepath, ec)) {
                return tool_error("file not found: " + filename);
            }

            auto content = read_file(filepath);
            return tool_result({{"name", name}, {"content", content}});
        };

        registry.register_tool(std::move(e));
    }
}

}  // namespace hermes::tools
