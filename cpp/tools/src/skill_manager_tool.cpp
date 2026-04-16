#include "hermes/tools/skill_manager_tool.hpp"

#include "hermes/core/path.hpp"
#include "hermes/llm/llm_client.hpp"
#include "hermes/skills/skills_hub.hpp"
#include "hermes/tools/registry.hpp"

#include <nlohmann/json.hpp>

#include <cstdlib>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <regex>
#include <sstream>
#include <string>
#include <unordered_set>
#include <vector>

namespace hermes::tools {

using json = nlohmann::json;
namespace fs = std::filesystem;

namespace {

std::string read_file_contents(const fs::path& p) {
    std::ifstream ifs(p);
    if (!ifs.is_open()) return {};
    std::ostringstream ss;
    ss << ifs.rdbuf();
    return ss.str();
}

std::string trim(std::string_view s) {
    auto begin = s.find_first_not_of(" \t\r\n");
    if (begin == std::string_view::npos) return {};
    auto end = s.find_last_not_of(" \t\r\n");
    return std::string(s.substr(begin, end - begin + 1));
}

std::string to_lower(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        out.push_back(static_cast<char>(std::tolower(
            static_cast<unsigned char>(c))));
    }
    return out;
}

}  // namespace

const std::vector<std::string>& allowed_skill_subdirs() {
    static const std::vector<std::string> v = {"references", "templates",
                                               "scripts", "assets"};
    return v;
}

fs::path skills_root() {
    return hermes::core::path::get_hermes_home() / "skills";
}

// ---- Validation ----------------------------------------------------------

std::string validate_skill_name(std::string_view name) {
    if (name.empty()) return "Skill name is required.";
    if (name.size() > kSkillMaxNameLength) {
        return "Skill name exceeds " + std::to_string(kSkillMaxNameLength) +
               " characters.";
    }
    static const std::regex re(R"(^[a-z0-9][a-z0-9._-]*$)");
    if (!std::regex_match(std::string(name), re)) {
        return "Invalid skill name '" + std::string(name) +
               "'. Use lowercase letters, numbers, hyphens, dots, and "
               "underscores.";
    }
    return {};
}

std::string validate_skill_category(std::string_view category) {
    auto t = trim(category);
    if (t.empty()) return {};
    if (t.size() > kSkillMaxNameLength) {
        return "Category exceeds " + std::to_string(kSkillMaxNameLength) +
               " characters.";
    }
    static const std::regex re(R"(^[a-z0-9][a-z0-9._-]*$)");
    if (!std::regex_match(t, re)) {
        return "Invalid category '" + t +
               "'. Use lowercase letters, numbers, hyphens, dots, and "
               "underscores.";
    }
    return {};
}

std::string validate_skill_file_path(std::string_view rel) {
    if (rel.empty()) return "File path is required.";
    fs::path p(std::string{rel});
    if (p.is_absolute()) return "File path must be relative.";
    for (const auto& part : p) {
        auto s = part.string();
        if (s == ".." || s == ".") {
            return "File path must not contain '..' or '.'.";
        }
    }
    auto first = p.begin();
    if (first == p.end()) return "File path is required.";
    auto subdir = first->string();
    const auto& allowed = allowed_skill_subdirs();
    if (std::find(allowed.begin(), allowed.end(), subdir) == allowed.end()) {
        return "File must live under one of: references/, templates/, "
               "scripts/, assets/";
    }
    return {};
}

// ---- Frontmatter ---------------------------------------------------------

namespace {

std::vector<std::string> parse_yaml_list(std::string_view body) {
    // Very small YAML-ish list parser: accepts both inline ``[a, b]`` and
    // a sequence of ``- value`` lines.  Strings are trimmed and quotes
    // stripped; quoted strings preserve internal commas.
    std::vector<std::string> out;
    auto trimmed = trim(body);
    if (trimmed.empty()) return out;
    if (trimmed.front() == '[' && trimmed.back() == ']') {
        auto inner = trimmed.substr(1, trimmed.size() - 2);
        std::stringstream ss(inner);
        std::string item;
        while (std::getline(ss, item, ',')) {
            auto t = trim(item);
            if (t.size() >= 2 && (t.front() == '"' || t.front() == '\'') &&
                t.back() == t.front()) {
                t = t.substr(1, t.size() - 2);
            }
            if (!t.empty()) out.push_back(t);
        }
    }
    return out;
}

void merge_kv(SkillFrontmatter& out, std::string_view key,
              std::string_view raw_value) {
    auto value = trim(raw_value);
    if (value.size() >= 2 &&
        (value.front() == '"' || value.front() == '\'') &&
        value.back() == value.front()) {
        value = value.substr(1, value.size() - 2);
    }
    if (key == "name") {
        out.name = value;
    } else if (key == "description") {
        out.description = value;
    } else if (key == "version") {
        out.version = value;
    } else if (key == "tags") {
        out.tags = parse_yaml_list(value);
    } else if (key == "required_credential_files") {
        out.required_credential_files = parse_yaml_list(value);
    } else {
        out.raw[std::string(key)] = value;
    }
}

}  // namespace

SkillFrontmatter parse_skill_frontmatter(std::string_view body) {
    SkillFrontmatter fm;
    fm.raw = json::object();

    // Look for a leading ``---\n`` and a closing ``---``.
    constexpr std::string_view marker = "---";
    if (body.size() < marker.size() ||
        body.compare(0, marker.size(), marker) != 0) {
        return fm;
    }
    // Skip the first line.
    auto nl = body.find('\n', marker.size());
    if (nl == std::string_view::npos) return fm;
    auto rest = body.substr(nl + 1);
    auto end = rest.find("\n---");
    std::string_view yaml = end == std::string_view::npos ? rest
                                                          : rest.substr(0, end);

    std::stringstream ss{std::string(yaml)};
    std::string line;
    while (std::getline(ss, line)) {
        auto t = trim(line);
        if (t.empty() || t.front() == '#') continue;
        auto colon = t.find(':');
        if (colon == std::string::npos) continue;
        merge_kv(fm, t.substr(0, colon), t.substr(colon + 1));
    }
    return fm;
}

SkillFrontmatter read_skill_frontmatter(const fs::path& skill_md) {
    return parse_skill_frontmatter(read_file_contents(skill_md));
}

// ---- Listing -------------------------------------------------------------

std::vector<InstalledSkill> enumerate_installed_skills(const fs::path& root) {
    std::vector<InstalledSkill> out;
    std::error_code ec;
    if (!fs::is_directory(root, ec)) return out;
    for (const auto& entry : fs::directory_iterator(root, ec)) {
        if (!entry.is_directory(ec)) continue;
        auto idx = entry.path() / "index.json";
        auto md = entry.path() / "SKILL.md";
        bool has_idx = fs::exists(idx, ec);
        bool has_md = fs::exists(md, ec);
        if (!has_idx && !has_md) continue;

        InstalledSkill s;
        s.name = entry.path().filename().string();
        s.path = entry.path();
        s.has_skill_md = has_md;
        s.has_index_json = has_idx;
        if (has_idx) {
            auto content = read_file_contents(idx);
            if (!content.empty()) {
                try {
                    auto j = json::parse(content);
                    s.description = j.value("description", "");
                    s.version = j.value("version", "");
                } catch (...) {}
            }
        }
        if (has_md && (s.description.empty() || s.version.empty())) {
            auto fm = read_skill_frontmatter(md);
            if (s.description.empty()) s.description = fm.description;
            if (s.version.empty()) s.version = fm.version;
        }
        out.push_back(std::move(s));
    }
    std::sort(out.begin(), out.end(),
              [](const InstalledSkill& a, const InstalledSkill& b) {
                  return a.name < b.name;
              });
    return out;
}

json render_installed_list(const std::vector<InstalledSkill>& installed) {
    json arr = json::array();
    for (const auto& s : installed) {
        arr.push_back(json{{"name", s.name},
                           {"description", s.description},
                           {"version", s.version},
                           {"path", s.path.string()}});
    }
    return json{{"skills", arr}, {"count", static_cast<int>(arr.size())}};
}

std::vector<InstalledSkill> search_installed_skills(
    const std::vector<InstalledSkill>& installed, std::string_view query) {
    std::vector<InstalledSkill> out;
    auto q = to_lower(query);
    if (q.empty()) return out;
    for (const auto& s : installed) {
        if (to_lower(s.name).find(q) != std::string::npos ||
            to_lower(s.description).find(q) != std::string::npos) {
            out.push_back(s);
        }
    }
    return out;
}

bool path_under_root(const fs::path& candidate, const fs::path& root) {
    std::error_code ec;
    auto c = fs::weakly_canonical(candidate, ec);
    if (ec) c = candidate;
    auto r = fs::weakly_canonical(root, ec);
    if (ec) r = root;
    auto cs = c.string();
    auto rs = r.string();
    if (cs.size() < rs.size()) return false;
    return cs.compare(0, rs.size(), rs) == 0;
}

// ---- Dispatch ------------------------------------------------------------

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
            auto installed = enumerate_installed_skills(skills_root());
            return tool_result(render_installed_list(installed));
        }

        if (action == "list_available") {
            const char* hub_url = std::getenv("HERMES_SKILLS_HUB_URL");
            const char* hub_token = std::getenv("HERMES_SKILLS_HUB_TOKEN");
            if (!hub_url || !*hub_url) {
                return tool_error("HERMES_SKILLS_HUB_URL not set");
            }
            hermes::skills::SkillsHub hub(hermes::skills::HubPaths::discover());
            hub.set_base_url(hub_url);
            int page = args.value("page", 1);
            int page_size = args.value("page_size", 50);
            std::string err;
            auto entries = hub.list_all(hub_token ? hub_token : "",
                                        page, page_size, &err);
            if (!err.empty()) return tool_error(err);
            json arr = json::array();
            for (const auto& e : entries) {
                arr.push_back(json{{"name", e.name},
                                   {"version", e.version},
                                   {"description", e.description},
                                   {"author", e.author},
                                   {"tags", e.tags},
                                   {"size_bytes", e.size_bytes},
                                   {"updated_at", e.updated_at}});
            }
            return tool_result(json{{"skills", arr},
                                    {"count", static_cast<int>(arr.size())},
                                    {"page", page},
                                    {"source", "hub"}});
        }

        if (action == "search") {
            auto query = args.value("query", "");
            if (query.empty()) return tool_error("missing required parameter: query");
            // Hub first when HERMES_SKILLS_HUB_URL is set; fall back to local.
            const char* hub_url = std::getenv("HERMES_SKILLS_HUB_URL");
            const char* hub_token = std::getenv("HERMES_SKILLS_HUB_TOKEN");
            auto* transport = hub_url && *hub_url
                                  ? hermes::llm::get_default_transport()
                                  : nullptr;
            if (transport) {
                std::unordered_map<std::string, std::string> h{{"Accept", "application/json"}};
                if (hub_token && *hub_token) h["Authorization"] = std::string("Bearer ") + hub_token;
                try {
                    auto resp = transport->get(std::string(hub_url) + "/skills/search?q=" + query, h);
                    if (resp.status_code >= 200 && resp.status_code < 300) {
                        auto root = json::parse(resp.body, nullptr, false);
                        if (!root.is_discarded()) {
                            json items = root.is_array() ? root
                                : (root.contains("items") ? root["items"] : json::array());
                            return tool_result(json{{"skills", items},
                                                    {"count", static_cast<int>(items.size())},
                                                    {"source", "hub"}});
                        }
                    }
                } catch (...) { /* fall through to local */ }
            }
            auto installed = enumerate_installed_skills(skills_root());
            return tool_result(render_installed_list(search_installed_skills(installed, query)));
        }

        if (action == "install") {
            auto name = args.value("name", "");
            if (name.empty()) return tool_error("missing required parameter: name");
            if (name.find('/') != std::string::npos ||
                name.find('\\') != std::string::npos ||
                name == ".." || name == ".") {
                return tool_error("invalid skill name: " + name);
            }
            const char* hub_url = std::getenv("HERMES_SKILLS_HUB_URL");
            const char* hub_token = std::getenv("HERMES_SKILLS_HUB_TOKEN");
            if (!hub_url || !*hub_url) {
                return tool_error("HERMES_SKILLS_HUB_URL not set");
            }
            hermes::skills::SkillsHub hub(hermes::skills::HubPaths::discover());
            hub.set_base_url(hub_url);
            std::string err;
            auto installed = hub.install(name, skills_root(),
                                         hub_token ? hub_token : "",
                                         &err);
            if (!installed) return tool_error(err.empty() ? "install failed" : err);
            return tool_result(json{{"installed", true},
                                    {"name", name},
                                    {"path", installed->string()}});
        }

        if (action == "uninstall") {
            auto name = args.value("name", "");
            if (name.empty()) {
                return tool_error("missing required parameter: name");
            }
            // Reject path separators and dotfiles outright (the regex
            // validator below would also catch these).
            if (name.find('/') != std::string::npos ||
                name.find('\\') != std::string::npos ||
                name == ".." || name == ".") {
                return tool_error("invalid skill name: " + name);
            }
            auto skill_dir = skills_root() / name;
            std::error_code ec;
            if (!path_under_root(skill_dir, skills_root())) {
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
            auto name = args.value("name", "");
            if (name.empty()) return tool_error("missing required parameter: name");
            if (name.find('/') != std::string::npos ||
                name.find('\\') != std::string::npos ||
                name == ".." || name == ".") {
                return tool_error("invalid skill name: " + name);
            }
            const char* hub_url = std::getenv("HERMES_SKILLS_HUB_URL");
            const char* hub_token = std::getenv("HERMES_SKILLS_HUB_TOKEN");
            if (!hub_url || !*hub_url) {
                return tool_error("HERMES_SKILLS_HUB_URL not set");
            }
            hermes::skills::SkillsHub hub(hermes::skills::HubPaths::discover());
            hub.set_base_url(hub_url);
            std::string err;
            auto installed = hub.install(name, skills_root(),
                                         hub_token ? hub_token : "",
                                         &err);
            if (!installed) return tool_error(err.empty() ? "update failed" : err);
            return tool_result(json{{"updated", true},
                                    {"name", name},
                                    {"path", installed->string()}});
        }

        return tool_error("unknown action: " + action);
    };

    registry.register_tool(std::move(e));
}

}  // namespace hermes::tools
