#include "hermes/skills/skill_utils.hpp"

#include "hermes/core/path.hpp"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>

namespace hermes::skills {

namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// Directory scanning
// ---------------------------------------------------------------------------

std::vector<fs::path> get_all_skills_dirs() {
    std::vector<fs::path> dirs;

    const auto home = hermes::core::path::get_hermes_home();

    // Built-in skills ship inside the install tree.
    auto builtin = home / "skills";
    std::error_code ec;
    if (fs::is_directory(builtin, ec)) {
        dirs.push_back(std::move(builtin));
    }

    // Optional skills (user-toggled).
    auto optional_dir = hermes::core::path::get_optional_skills_dir();
    if (fs::is_directory(optional_dir, ec)) {
        dirs.push_back(std::move(optional_dir));
    }

    // User-installed skills.
    auto user_dir = home / "installed-skills";
    if (fs::is_directory(user_dir, ec)) {
        dirs.push_back(std::move(user_dir));
    }

    // Fallback search paths for built-in skills shipped alongside the
    // hermes_cpp binary. These cover three common layouts:
    //   1. System install: /usr/share/hermes/{skills,optional-skills}
    //   2. Local install:  /usr/local/share/hermes/{skills,optional-skills}
    //   3. Dev checkout / custom path via HERMES_SKILLS_SEARCH_PATH
    //      (colon-separated list of directories).
    auto add_if_dir = [&](const fs::path& p) {
        std::error_code lec;
        if (!fs::is_directory(p, lec)) return;
        for (const auto& existing : dirs) {
            std::error_code eq_ec;
            if (fs::equivalent(existing, p, eq_ec)) return;
        }
        dirs.push_back(p);
    };

    if (const char* extra_env = std::getenv("HERMES_SKILLS_SEARCH_PATH")) {
        std::string s(extra_env);
        std::string::size_type start = 0;
        while (start <= s.size()) {
            auto sep = s.find(':', start);
            std::string piece = s.substr(start, sep - start);
            if (!piece.empty()) add_if_dir(fs::path(piece));
            if (sep == std::string::npos) break;
            start = sep + 1;
        }
    }

    for (const char* prefix :
         {"/usr/share/hermes", "/usr/local/share/hermes"}) {
        add_if_dir(fs::path(prefix) / "skills");
        add_if_dir(fs::path(prefix) / "optional-skills");
        // Self-contained built-in skill collection shipped alongside the
        // C++ binary — the same SKILL.md files as the Python skills/ tree,
        // copied at build time for distributions where the Python repo is
        // absent (Termux, container-only, Windows).
        add_if_dir(fs::path(prefix) / "builtins");
    }

    // Termux install prefix — resolved at runtime from $PREFIX so that
    // builtins also ship under $PREFIX/share/hermes on Android.
    if (const char* termux_prefix = std::getenv("PREFIX")) {
        fs::path tp(termux_prefix);
        add_if_dir(tp / "share" / "hermes" / "skills");
        add_if_dir(tp / "share" / "hermes" / "optional-skills");
        add_if_dir(tp / "share" / "hermes" / "builtins");
    }

    // HERMES_BUILTINS_DIR — explicit override, used by the builtins-parity
    // test to point the skill index at cpp/skills/builtins/ in a dev
    // checkout without requiring `make install`.
    if (const char* builtins_env = std::getenv("HERMES_BUILTINS_DIR")) {
        add_if_dir(fs::path(builtins_env));
    }

    return dirs;
}

// ---------------------------------------------------------------------------
// Frontmatter parsing
// ---------------------------------------------------------------------------

static constexpr std::string_view kFence = "---";

std::pair<nlohmann::json, std::string> parse_frontmatter(std::string_view markdown) {
    // The frontmatter must start at position 0 with "---\n".
    if (markdown.size() < 4 || markdown.substr(0, 3) != kFence ||
        markdown[3] != '\n') {
        return {nlohmann::json(nullptr), std::string(markdown)};
    }

    // Find the closing fence.
    auto close = markdown.find("\n---", 3);
    if (close == std::string_view::npos) {
        return {nlohmann::json(nullptr), std::string(markdown)};
    }

    // Extract the YAML block (between the two fences).
    std::string_view yaml_block = markdown.substr(4, close - 4);

    // Lightweight key: value parser (no dependency on yaml-cpp).
    // Handles simple scalars and arrays expressed as comma-separated lists.
    nlohmann::json meta = nlohmann::json::object();
    std::istringstream ss{std::string(yaml_block)};
    std::string line;
    while (std::getline(ss, line)) {
        auto colon = line.find(':');
        if (colon == std::string::npos) continue;

        std::string key = line.substr(0, colon);
        // Trim key
        while (!key.empty() && key.back() == ' ') key.pop_back();
        while (!key.empty() && key.front() == ' ') key.erase(key.begin());

        std::string val = line.substr(colon + 1);
        // Trim value
        while (!val.empty() && val.front() == ' ') val.erase(val.begin());
        while (!val.empty() && val.back() == ' ') val.pop_back();

        if (key.empty()) continue;

        // Detect comma-separated arrays: "cli, telegram"
        if (val.find(',') != std::string::npos) {
            nlohmann::json arr = nlohmann::json::array();
            std::istringstream vs(val);
            std::string item;
            while (std::getline(vs, item, ',')) {
                while (!item.empty() && item.front() == ' ') item.erase(item.begin());
                while (!item.empty() && item.back() == ' ') item.pop_back();
                if (!item.empty()) arr.push_back(item);
            }
            meta[key] = std::move(arr);
        } else {
            meta[key] = val;
        }
    }

    // Body starts after the closing fence line.
    auto body_start = close + 4;  // skip "\n---"
    if (body_start < markdown.size() && markdown[body_start] == '\n') {
        ++body_start;
    }
    std::string body(markdown.substr(body_start));

    return {std::move(meta), std::move(body)};
}

// ---------------------------------------------------------------------------
// Frontmatter extraction helpers
// ---------------------------------------------------------------------------

std::string extract_skill_description(const nlohmann::json& frontmatter) {
    if (frontmatter.is_null()) return {};
    if (frontmatter.contains("description") && frontmatter["description"].is_string()) {
        return frontmatter["description"].get<std::string>();
    }
    return {};
}

std::vector<std::string> extract_skill_conditions(const nlohmann::json& frontmatter) {
    std::vector<std::string> out;
    if (frontmatter.is_null()) return out;
    if (frontmatter.contains("conditions")) {
        const auto& c = frontmatter["conditions"];
        if (c.is_array()) {
            for (const auto& item : c) {
                if (item.is_string()) {
                    out.push_back(item.get<std::string>());
                }
            }
        } else if (c.is_string()) {
            // Single condition as comma list.
            std::istringstream ss(c.get<std::string>());
            std::string tok;
            while (std::getline(ss, tok, ',')) {
                while (!tok.empty() && tok.front() == ' ') tok.erase(tok.begin());
                while (!tok.empty() && tok.back() == ' ') tok.pop_back();
                if (!tok.empty()) out.push_back(tok);
            }
        }
    }
    return out;
}

// ---------------------------------------------------------------------------
// Skill index iteration
// ---------------------------------------------------------------------------

std::vector<SkillMetadata> iter_skill_index() {
    std::vector<SkillMetadata> result;

    auto load_skill = [&](const fs::path& dir, const std::string& display_name) {
        std::error_code ec;
        auto skill_md = dir / "SKILL.md";
        if (!fs::is_regular_file(skill_md, ec)) return;
        std::ifstream ifs(skill_md);
        if (!ifs) return;
        std::string contents((std::istreambuf_iterator<char>(ifs)),
                              std::istreambuf_iterator<char>());
        auto [meta, body] = parse_frontmatter(contents);
        SkillMetadata sm;
        sm.name = display_name;
        sm.path = dir;
        sm.description = extract_skill_description(meta);
        if (!meta.is_null()) {
            if (meta.contains("version") && meta["version"].is_string()) {
                sm.version = meta["version"].get<std::string>();
            }
            if (meta.contains("platforms")) {
                const auto& p = meta["platforms"];
                if (p.is_array()) {
                    for (const auto& item : p) {
                        if (item.is_string()) sm.platforms.push_back(item.get<std::string>());
                    }
                } else if (p.is_string()) {
                    sm.platforms.push_back(p.get<std::string>());
                }
            }
            if (meta.contains("categories") && meta["categories"].is_array()) {
                for (const auto& item : meta["categories"]) {
                    if (item.is_string()) sm.categories.push_back(item.get<std::string>());
                }
            }
        }
        result.push_back(std::move(sm));
    };

    // Walk up to 3 levels — Python hermes-agent's skills/ uses any of:
    //   * flat layout:            skills/<name>/SKILL.md
    //   * category layout:        skills/<category>/<name>/SKILL.md
    //   * nested-category layout: skills/<category>/<subcat>/<name>/SKILL.md
    //                             (e.g. mlops/inference/vllm/SKILL.md).
    // Each layout is discovered by peeking for SKILL.md at the leaf.
    for (const auto& dir : get_all_skills_dirs()) {
        std::error_code ec;
        for (const auto& entry : fs::directory_iterator(dir, ec)) {
            if (!entry.is_directory(ec)) continue;
            auto sub = entry.path();
            std::string sub_name = sub.filename().string();
            // Layout A: skills/<name>/SKILL.md
            if (fs::is_regular_file(sub / "SKILL.md", ec)) {
                load_skill(sub, sub_name);
                continue;
            }
            // Layout B or C: skills/<category>/...
            for (const auto& sub_entry : fs::directory_iterator(sub, ec)) {
                if (!sub_entry.is_directory(ec)) continue;
                auto leaf = sub_entry.path();
                std::string leaf_name = leaf.filename().string();
                // Layout B: skills/<category>/<name>/SKILL.md
                if (fs::is_regular_file(leaf / "SKILL.md", ec)) {
                    load_skill(leaf, sub_name + "/" + leaf_name);
                    continue;
                }
                // Layout C: skills/<category>/<subcat>/<name>/SKILL.md
                for (const auto& leaf_entry : fs::directory_iterator(leaf, ec)) {
                    if (!leaf_entry.is_directory(ec)) continue;
                    load_skill(leaf_entry.path(),
                               sub_name + "/" + leaf_name + "/" +
                               leaf_entry.path().filename().string());
                }
            }
        }
    }

    return result;
}

// ---------------------------------------------------------------------------
// Platform gating
// ---------------------------------------------------------------------------

bool skill_matches_platform(const SkillMetadata& skill, std::string_view platform) {
    if (skill.platforms.empty()) return true;
    return std::any_of(skill.platforms.begin(), skill.platforms.end(),
                       [&](const std::string& p) {
                           return p == platform;
                       });
}

// ---------------------------------------------------------------------------
// Disabled skills from config
// ---------------------------------------------------------------------------

std::vector<std::string> get_disabled_skill_names(const nlohmann::json& config) {
    std::vector<std::string> out;
    if (!config.is_object()) return out;
    if (!config.contains("disabled_skills")) return out;

    const auto& ds = config["disabled_skills"];
    if (ds.is_array()) {
        for (const auto& item : ds) {
            if (item.is_string()) {
                out.push_back(item.get<std::string>());
            }
        }
    }
    return out;
}

}  // namespace hermes::skills
