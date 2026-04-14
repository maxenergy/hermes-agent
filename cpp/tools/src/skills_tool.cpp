// Skills tools — port of tools/skills_tool.py.
//
// Implements skills_list, skill_view, skills_categories plus the small
// parsing utilities the Python module exposes (frontmatter, platform
// matching, category extraction, token estimation, tag parsing).  The
// helpers are exposed in the public header so tests can cover them
// directly without round-tripping through the registry.
#include "hermes/tools/skills_tool.hpp"

#include "hermes/core/path.hpp"
#include "hermes/tools/registry.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace hermes::tools::skills {

namespace fs = std::filesystem;

namespace {

// Directories inside ~/.hermes/skills that must never be treated as a
// skill when crawling.  Mirrors _EXCLUDED_SKILL_DIRS in the Python.
const std::unordered_set<std::string>& excluded_dirs() {
    static const std::unordered_set<std::string> s{
        "node_modules", "__pycache__", ".git", ".venv", "venv",
        "dist", "build", ".pytest_cache", ".mypy_cache",
    };
    return s;
}

// Read a file fully.  Returns empty on error.
std::string read_file(const fs::path& p,
                      std::size_t max_bytes = std::string::npos) {
    std::ifstream ifs(p, std::ios::binary);
    if (!ifs.is_open()) return {};
    std::ostringstream ss;
    ss << ifs.rdbuf();
    std::string s = ss.str();
    if (max_bytes != std::string::npos && s.size() > max_bytes) {
        s.resize(max_bytes);
    }
    return s;
}

std::string rtrim_copy(std::string s) {
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' ||
                          s.back() == '\r' || s.back() == '\n')) {
        s.pop_back();
    }
    return s;
}

std::string ltrim_copy(std::string s) {
    std::size_t i = 0;
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i;
    return s.substr(i);
}

std::string trim_copy(std::string s) {
    s = rtrim_copy(std::move(s));
    return ltrim_copy(std::move(s));
}

// Strip optional matching quotes around a scalar value.
std::string strip_quotes(std::string s) {
    s = trim_copy(std::move(s));
    if (s.size() >= 2 &&
        ((s.front() == '"' && s.back() == '"') ||
         (s.front() == '\'' && s.back() == '\''))) {
        return s.substr(1, s.size() - 2);
    }
    return s;
}

// Parse "[a, b, 'c']" into a JSON array of strings.
nlohmann::json parse_inline_list(std::string_view raw) {
    std::string s(raw);
    s = trim_copy(std::move(s));
    if (s.size() >= 2 && s.front() == '[' && s.back() == ']') {
        s = s.substr(1, s.size() - 2);
    }
    nlohmann::json arr = nlohmann::json::array();
    std::string cur;
    int depth = 0;
    for (char c : s) {
        if (c == ',' && depth == 0) {
            auto item = strip_quotes(trim_copy(cur));
            if (!item.empty()) arr.push_back(item);
            cur.clear();
        } else {
            if (c == '[' || c == '{') ++depth;
            if (c == ']' || c == '}') --depth;
            cur += c;
        }
    }
    auto item = strip_quotes(trim_copy(cur));
    if (!item.empty()) arr.push_back(item);
    return arr;
}

// The current OS tag used by platforms-frontmatter matching.
std::string current_platform_tag() {
#if defined(__APPLE__)
    return "macos";
#elif defined(_WIN32)
    return "windows";
#else
    return "linux";
#endif
}

}  // namespace

fs::path skills_dir() {
    return hermes::core::path::get_hermes_home() / "skills";
}

std::pair<nlohmann::json, std::string> parse_frontmatter(
    std::string_view content) {
    nlohmann::json fm = nlohmann::json::object();
    // Must start with '---\n'.
    if (content.size() < 4 || content.substr(0, 4) != "---\n") {
        return {fm, std::string(content)};
    }

    // Find the closing '\n---\n' (or '\n---' at EOF).
    std::size_t start = 4;
    std::size_t end = std::string::npos;
    for (std::size_t i = start; i + 3 <= content.size(); ++i) {
        if (content[i] == '\n' && content.compare(i + 1, 3, "---") == 0) {
            // Accept either end-of-string or a newline after.
            if (i + 4 == content.size() || content[i + 4] == '\n' ||
                content[i + 4] == '\r') {
                end = i + 1;
                break;
            }
        }
    }
    if (end == std::string::npos) {
        return {fm, std::string(content)};
    }

    std::string fm_block(content.substr(start, end - start));
    // Body starts after '---\n' past the end marker.
    std::size_t body_start = end + 3;
    while (body_start < content.size() &&
           (content[body_start] == '\n' || content[body_start] == '\r')) {
        ++body_start;
    }
    std::string body(content.substr(body_start));

    // Parse the frontmatter line by line.  Very simple YAML subset.
    std::istringstream iss(fm_block);
    std::string line;
    std::string pending_key;
    std::vector<std::string> pending_list;

    auto flush_pending = [&]() {
        if (!pending_key.empty()) {
            nlohmann::json arr = nlohmann::json::array();
            for (auto& v : pending_list) arr.push_back(v);
            fm[pending_key] = arr;
            pending_key.clear();
            pending_list.clear();
        }
    };

    while (std::getline(iss, line)) {
        // Drop trailing CR.
        if (!line.empty() && line.back() == '\r') line.pop_back();

        std::string stripped = line;
        std::size_t leading_ws = 0;
        while (leading_ws < stripped.size() &&
               (stripped[leading_ws] == ' ' || stripped[leading_ws] == '\t')) {
            ++leading_ws;
        }

        // Comment or blank.
        if (leading_ws == stripped.size() || stripped[leading_ws] == '#') {
            continue;
        }

        // Nested list item ("  - foo") for a pending block list.
        if (!pending_key.empty() && leading_ws >= 2 &&
            stripped.size() > leading_ws && stripped[leading_ws] == '-') {
            std::string val = stripped.substr(leading_ws + 1);
            pending_list.push_back(strip_quotes(trim_copy(val)));
            continue;
        }

        // A top-level line closes any pending block list.
        flush_pending();

        // key: value
        auto colon = stripped.find(':', leading_ws);
        if (colon == std::string::npos) continue;
        std::string key =
            trim_copy(stripped.substr(leading_ws, colon - leading_ws));
        std::string value =
            trim_copy(stripped.substr(colon + 1));

        if (key.empty()) continue;

        if (value.empty()) {
            // Begin a potential block list or mapping.  We only support
            // block lists here.
            pending_key = key;
            pending_list.clear();
            continue;
        }

        if (!value.empty() && value.front() == '[') {
            fm[key] = parse_inline_list(value);
        } else {
            fm[key] = strip_quotes(value);
        }
    }
    flush_pending();

    return {fm, body};
}

bool skill_matches_platform(const nlohmann::json& fm) {
    if (!fm.is_object() || !fm.contains("platforms")) return true;
    auto platforms = fm["platforms"];
    auto current = current_platform_tag();
    if (platforms.is_array()) {
        for (const auto& p : platforms) {
            if (p.is_string() && p.get<std::string>() == current) return true;
        }
        return false;
    }
    if (platforms.is_string()) {
        return platforms.get<std::string>() == current;
    }
    return true;
}

std::string get_category_from_path(const fs::path& skill_md) {
    std::error_code ec;
    auto base = skills_dir();
    auto rel = fs::relative(skill_md, base, ec);
    if (ec) return {};
    // category/skill/SKILL.md → 3 parts.
    int parts = 0;
    fs::path first;
    for (const auto& seg : rel) {
        if (parts == 0) first = seg;
        ++parts;
    }
    if (parts >= 3) return first.string();
    return {};
}

std::size_t estimate_tokens(std::string_view content) {
    return content.size() / 4;
}

std::vector<std::string> parse_tags(const nlohmann::json& v) {
    std::vector<std::string> out;
    if (v.is_null()) return out;
    if (v.is_array()) {
        for (const auto& t : v) {
            if (t.is_string()) {
                auto s = trim_copy(t.get<std::string>());
                if (!s.empty()) out.push_back(std::move(s));
            } else {
                out.push_back(t.dump());
            }
        }
        return out;
    }
    if (v.is_string()) {
        std::string s = trim_copy(v.get<std::string>());
        if (s.size() >= 2 && s.front() == '[' && s.back() == ']') {
            s = s.substr(1, s.size() - 2);
        }
        std::string cur;
        for (char c : s) {
            if (c == ',') {
                auto tag = strip_quotes(trim_copy(cur));
                if (!tag.empty()) out.push_back(std::move(tag));
                cur.clear();
            } else {
                cur += c;
            }
        }
        auto tag = strip_quotes(trim_copy(cur));
        if (!tag.empty()) out.push_back(std::move(tag));
    }
    return out;
}

namespace {

// Extract the first non-blank, non-header line from the skill body.
std::string first_paragraph(std::string_view body) {
    std::istringstream iss{std::string(body)};
    std::string line;
    while (std::getline(iss, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        auto t = trim_copy(line);
        if (t.empty()) continue;
        if (!t.empty() && t.front() == '#') continue;
        return t;
    }
    return {};
}

// Build a skill metadata JSON entry for a given SKILL.md.
// Returns a null json when the skill is excluded.
nlohmann::json build_skill_entry(const fs::path& skill_md) {
    auto raw = read_file(skill_md, 4000);
    if (raw.empty()) return {};

    auto [fm, body] = parse_frontmatter(raw);
    if (!skill_matches_platform(fm)) return {};

    std::string name;
    if (fm.contains("name") && fm["name"].is_string()) {
        name = fm["name"].get<std::string>();
    }
    if (name.empty()) {
        name = skill_md.parent_path().filename().string();
    }
    if (name.size() > kMaxNameLength) name.resize(kMaxNameLength);

    std::string description;
    if (fm.contains("description") && fm["description"].is_string()) {
        description = fm["description"].get<std::string>();
    }
    if (description.empty()) description = first_paragraph(body);
    if (description.size() > kMaxDescriptionLength) {
        description = description.substr(0, kMaxDescriptionLength - 3) + "...";
    }

    nlohmann::json entry;
    entry["name"] = name;
    entry["description"] = description;
    auto category = get_category_from_path(skill_md);
    if (!category.empty()) entry["category"] = category;
    return entry;
}

// Recursively walk the skills dir and collect SKILL.md files.
std::vector<fs::path> find_skill_mds(const fs::path& root) {
    std::vector<fs::path> out;
    std::error_code ec;
    if (!fs::is_directory(root, ec)) return out;
    for (auto it = fs::recursive_directory_iterator(
             root, fs::directory_options::skip_permission_denied, ec);
         !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
        const auto& p = it->path();
        const auto name = p.filename().string();
        if (it->is_directory(ec)) {
            if (excluded_dirs().count(name)) {
                it.disable_recursion_pending();
            }
            continue;
        }
        if (name != "SKILL.md") continue;
        // Skip anything under an excluded ancestor.
        bool excluded = false;
        for (const auto& part : p) {
            if (excluded_dirs().count(part.filename().string())) {
                excluded = true;
                break;
            }
        }
        if (excluded) continue;
        out.push_back(p);
    }
    return out;
}

// Locate a skill directory by name — tries direct path, then searches
// for any SKILL.md whose containing directory matches.  Returns the
// skill directory path (or empty if not found).
fs::path locate_skill(const std::string& name) {
    auto base = skills_dir();
    std::error_code ec;
    if (auto direct = base / name; fs::is_directory(direct, ec)) {
        if (fs::exists(direct / "SKILL.md", ec) ||
            fs::exists(direct / "index.json", ec)) {
            return direct;
        }
    }
    for (const auto& md : find_skill_mds(base)) {
        auto parent = md.parent_path();
        if (parent.filename().string() == name) return parent;
        // Also check the frontmatter 'name' field.
        auto raw = read_file(md, 4000);
        auto [fm, _] = parse_frontmatter(raw);
        if (fm.contains("name") && fm["name"].is_string() &&
            fm["name"].get<std::string>() == name) {
            return parent;
        }
    }
    return {};
}

}  // namespace

void register_skills_tools(hermes::tools::ToolRegistry& registry) {
    using json = nlohmann::json;

    // ----- skills_list --------------------------------------------------
    {
        ToolEntry e;
        e.name = "skills_list";
        e.toolset = "skills";
        e.description = "List installed skills (tier-1 disclosure)";
        e.emoji = "\xf0\x9f\x93\x9a";  // books
        e.schema = json::parse(R"JSON({
            "type": "object",
            "properties": {
                "category": {
                    "type": "string",
                    "description": "Optional category filter"
                }
            }
        })JSON");

        e.handler = [](const json& args, const ToolContext&) -> std::string {
            const auto base = skills_dir();
            std::error_code ec;
            if (!fs::is_directory(base, ec)) {
                fs::create_directories(base, ec);
                return tool_result({
                    {"success", true},
                    {"skills", json::array()},
                    {"categories", json::array()},
                    {"count", 0},
                    {"message",
                     "No skills found. Skills directory created."}});
            }

            std::string category;
            if (args.contains("category") && args["category"].is_string()) {
                category = args["category"].get<std::string>();
            }

            json skills = json::array();
            std::unordered_set<std::string> seen;
            std::unordered_set<std::string> cats;

            for (const auto& md : find_skill_mds(base)) {
                auto entry = build_skill_entry(md);
                if (entry.is_null()) continue;
                if (!entry.contains("name")) continue;
                auto name = entry["name"].get<std::string>();
                if (!seen.insert(name).second) continue;
                if (!category.empty()) {
                    if (!entry.contains("category")) continue;
                    if (entry["category"].get<std::string>() != category) {
                        continue;
                    }
                }
                if (entry.contains("category")) {
                    cats.insert(entry["category"].get<std::string>());
                }
                skills.push_back(std::move(entry));
            }

            // Sort by (category, name).
            std::sort(skills.begin(), skills.end(),
                      [](const json& a, const json& b) {
                          std::string ca = a.value("category", "");
                          std::string cb = b.value("category", "");
                          if (ca != cb) return ca < cb;
                          return a.value("name", "") < b.value("name", "");
                      });

            json categories = json::array();
            for (auto& c : cats) categories.push_back(c);
            std::sort(categories.begin(), categories.end());

            return tool_result({
                {"success", true},
                {"skills", skills},
                {"categories", categories},
                {"count", static_cast<int>(skills.size())},
                {"hint",
                 "Use skill_view(name) to load full content and linked files"}});
        };
        registry.register_tool(std::move(e));
    }

    // ----- skill_view ---------------------------------------------------
    {
        ToolEntry e;
        e.name = "skill_view";
        e.toolset = "skills";
        e.description =
            "View SKILL.md or a sub-file of a skill (tier-2 disclosure)";
        e.emoji = "\xf0\x9f\x93\x96";  // open book
        e.schema = json::parse(R"JSON({
            "type": "object",
            "properties": {
                "name": {
                    "type": "string",
                    "description": "Skill name (directory or frontmatter name)"
                },
                "file": {
                    "type": "string",
                    "description": "Optional sub-file (default SKILL.md)"
                },
                "file_path": {
                    "type": "string",
                    "description": "Alias for 'file' — Python-compat parameter"
                }
            },
            "required": ["name"]
        })JSON");

        e.handler = [](const json& args, const ToolContext&) -> std::string {
            if (!args.contains("name") || !args["name"].is_string()) {
                return tool_error("missing required parameter: name");
            }
            auto name = args["name"].get<std::string>();

            auto dir = locate_skill(name);
            std::error_code ec;
            if (dir.empty() || !fs::is_directory(dir, ec)) {
                return tool_error("skill not found: " + name);
            }

            std::string filename = "SKILL.md";
            if (args.contains("file") && args["file"].is_string()) {
                filename = args["file"].get<std::string>();
            } else if (args.contains("file_path") &&
                       args["file_path"].is_string()) {
                filename = args["file_path"].get<std::string>();
            }

            // Prevent path-escape.
            if (filename.find("..") != std::string::npos) {
                return tool_error("invalid file path: " + filename);
            }

            auto filepath = dir / filename;
            if (!fs::exists(filepath, ec)) {
                return tool_error("file not found: " + filename);
            }

            auto content = read_file(filepath);
            auto [fm, body] = parse_frontmatter(content);

            json result;
            result["success"] = true;
            result["name"] = name;
            result["content"] = content;
            result["file"] = filename;
            result["estimated_tokens"] =
                static_cast<int>(estimate_tokens(content));
            if (fm.is_object() && !fm.empty()) {
                result["frontmatter"] = fm;
                if (fm.contains("tags")) {
                    json tags_arr = json::array();
                    for (auto& t : parse_tags(fm["tags"])) {
                        tags_arr.push_back(t);
                    }
                    result["tags"] = tags_arr;
                }
            }
            return tool_result(result);
        };
        registry.register_tool(std::move(e));
    }

    // ----- skills_categories -------------------------------------------
    {
        ToolEntry e;
        e.name = "skills_categories";
        e.toolset = "skills";
        e.description =
            "List skill categories with per-category skill counts "
            "(tier-0 disclosure)";
        e.emoji = "\xf0\x9f\x97\x82\xef\xb8\x8f";  // card index dividers
        e.schema = json::parse(R"JSON({
            "type": "object",
            "properties": {
                "verbose": {
                    "type": "boolean",
                    "description": "Include skill counts (default true)"
                }
            }
        })JSON");

        e.handler = [](const json&, const ToolContext&) -> std::string {
            const auto base = skills_dir();
            std::error_code ec;
            if (!fs::is_directory(base, ec)) {
                return tool_result({
                    {"success", true},
                    {"categories", json::array()},
                    {"message", "No skills directory found"}});
            }

            std::unordered_map<std::string, int> counts;
            std::unordered_map<std::string, fs::path> cat_dir;
            for (const auto& md : find_skill_mds(base)) {
                auto cat = get_category_from_path(md);
                if (cat.empty()) continue;
                auto raw = read_file(md, 4000);
                auto [fm, _] = parse_frontmatter(raw);
                if (!skill_matches_platform(fm)) continue;
                ++counts[cat];
                if (!cat_dir.count(cat)) {
                    cat_dir[cat] = md.parent_path().parent_path();
                }
            }

            std::vector<std::string> names;
            names.reserve(counts.size());
            for (auto& [k, _] : counts) names.push_back(k);
            std::sort(names.begin(), names.end());

            json categories = json::array();
            for (const auto& name : names) {
                json entry = {{"name", name}, {"skill_count", counts[name]}};
                // Look for a DESCRIPTION.md file in the category dir.
                auto desc_file = cat_dir[name] / "DESCRIPTION.md";
                if (fs::exists(desc_file, ec)) {
                    auto raw = read_file(desc_file, 4000);
                    auto [fm, body] = parse_frontmatter(raw);
                    std::string desc;
                    if (fm.contains("description") &&
                        fm["description"].is_string()) {
                        desc = fm["description"].get<std::string>();
                    }
                    if (desc.empty()) desc = first_paragraph(body);
                    if (desc.size() > kMaxDescriptionLength) {
                        desc = desc.substr(0, kMaxDescriptionLength - 3) +
                               "...";
                    }
                    if (!desc.empty()) entry["description"] = desc;
                }
                categories.push_back(std::move(entry));
            }

            return tool_result({
                {"success", true},
                {"categories", categories},
                {"hint",
                 "Pick a relevant category then call skills_list with it"}});
        };
        registry.register_tool(std::move(e));
    }
}

}  // namespace hermes::tools::skills
