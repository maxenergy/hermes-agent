// C++17 port of ``agent/skill_utils.py`` — frontmatter parsing, skill
// directory iteration, disabled-skill resolution, and config-variable
// discovery.  Uses yaml-cpp via the config layer's dependency graph.
#include "hermes/agent/skill_utils.hpp"

#include "hermes/core/logging.hpp"
#include "hermes/core/path.hpp"

#include <yaml-cpp/yaml.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <system_error>

namespace hermes::agent::skill_utils {

namespace {

// ---------------------------------------------------------------------------
// String helpers.
// ---------------------------------------------------------------------------

std::string strip(const std::string& s) {
    auto is_space = [](unsigned char c) { return std::isspace(c); };
    auto begin = s.begin();
    while (begin != s.end() && is_space(*begin)) ++begin;
    auto end = s.end();
    while (end != begin && is_space(*(end - 1))) --end;
    return std::string(begin, end);
}

std::string strip_quotes(const std::string& s) {
    if (s.size() >= 2) {
        char first = s.front();
        char last = s.back();
        if ((first == '\'' && last == '\'') ||
            (first == '"' && last == '"')) {
            return s.substr(1, s.size() - 2);
        }
    }
    return s;
}

std::string to_lower_copy(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    return s;
}

bool starts_with(const std::string& s, const std::string& prefix) {
    return s.size() >= prefix.size() &&
           std::equal(prefix.begin(), prefix.end(), s.begin());
}

std::string read_file(const std::filesystem::path& p) {
    std::ifstream ifs(p, std::ios::binary);
    if (!ifs) return {};
    std::ostringstream ss;
    ss << ifs.rdbuf();
    return ss.str();
}

// ---------------------------------------------------------------------------
// yaml-cpp → nlohmann::json conversion (duplicated from config/loader.cpp's
// anonymous namespace; keeps this translation unit self-contained).
// ---------------------------------------------------------------------------

nlohmann::json yaml_node_to_json(const YAML::Node& node) {
    switch (node.Type()) {
        case YAML::NodeType::Null:
            return nullptr;
        case YAML::NodeType::Scalar: {
            const auto& s = node.Scalar();
            if (s == "null" || s == "~" || s.empty()) return nullptr;
            if (s == "true" || s == "True" || s == "TRUE") return true;
            if (s == "false" || s == "False" || s == "FALSE") return false;
            try {
                std::size_t pos = 0;
                const long long v = std::stoll(s, &pos);
                if (pos == s.size()) return v;
            } catch (...) {}
            try {
                std::size_t pos = 0;
                const double v = std::stod(s, &pos);
                if (pos == s.size()) return v;
            } catch (...) {}
            return s;
        }
        case YAML::NodeType::Sequence: {
            nlohmann::json arr = nlohmann::json::array();
            for (const auto& child : node) {
                arr.push_back(yaml_node_to_json(child));
            }
            return arr;
        }
        case YAML::NodeType::Map: {
            nlohmann::json obj = nlohmann::json::object();
            for (const auto& kv : node) {
                obj[kv.first.as<std::string>()] =
                    yaml_node_to_json(kv.second);
            }
            return obj;
        }
        case YAML::NodeType::Undefined:
        default:
            return nullptr;
    }
}

// ---------------------------------------------------------------------------
// Session platform resolution.  The Python module reads from
// ``gateway.session_context.get_session_env``; here we consult the two
// env vars in the same precedence order.
// ---------------------------------------------------------------------------

std::string getenv_str(const char* key) {
    const char* v = std::getenv(key);
    return v ? std::string(v) : std::string();
}

std::string resolve_session_platform(const std::string& override_platform) {
    if (!override_platform.empty()) return override_platform;
    auto hp = getenv_str("HERMES_PLATFORM");
    if (!hp.empty()) return hp;
    return getenv_str("HERMES_SESSION_PLATFORM");
}

}  // namespace

// ---------------------------------------------------------------------------
// Public: platform map + excluded dirs.
// ---------------------------------------------------------------------------

const std::unordered_set<std::string>& excluded_skill_dirs() {
    static const std::unordered_set<std::string> s = {".git", ".github",
                                                      ".hub"};
    return s;
}

std::string normalize_platform_name(const std::string& raw) {
    auto s = to_lower_copy(strip(raw));
    if (s == "macos") return "darwin";
    if (s == "linux") return "linux";
    if (s == "windows") return "win32";
    return s;
}

std::string current_platform_id() {
#if defined(__APPLE__)
    return "darwin";
#elif defined(_WIN32)
    return "win32";
#elif defined(__linux__)
    return "linux";
#else
    return "unknown";
#endif
}

// ---------------------------------------------------------------------------
// YAML parsing.
// ---------------------------------------------------------------------------

std::optional<nlohmann::json> parse_yaml(const std::string& content) {
    try {
        YAML::Node root = YAML::Load(content);
        return yaml_node_to_json(root);
    } catch (const std::exception& e) {
        hermes::core::logging::log_debug(
            std::string("skill_utils.parse_yaml: ") + e.what());
        return std::nullopt;
    }
}

Frontmatter parse_frontmatter(const std::string& content) {
    Frontmatter fm;
    fm.data = nlohmann::json::object();
    fm.body = content;

    // Must start with "---"
    if (!starts_with(content, "---")) {
        return fm;
    }

    // Find closing "\n---\s*\n" after offset 3.
    // Python uses: re.search(r"\n---\s*\n", content[3:])
    static const std::regex kClose(R"(\n---[^\S\n]*\n)");
    std::smatch m;
    auto tail_start = content.begin() + 3;
    std::string tail(tail_start, content.end());
    if (!std::regex_search(tail, m, kClose)) {
        return fm;
    }
    std::size_t yaml_len = static_cast<std::size_t>(m.position(0));
    std::string yaml_content = content.substr(3, yaml_len);
    fm.body = content.substr(3 + yaml_len + m.length(0));

    auto parsed = parse_yaml(yaml_content);
    if (parsed && parsed->is_object()) {
        fm.data = *parsed;
    } else {
        // Fallback: naive key:value split.
        std::istringstream iss(yaml_content);
        std::string line;
        while (std::getline(iss, line)) {
            auto stripped = strip(line);
            if (stripped.empty()) continue;
            auto colon = stripped.find(':');
            if (colon == std::string::npos) continue;
            auto key = strip(stripped.substr(0, colon));
            auto val = strip(stripped.substr(colon + 1));
            if (!key.empty()) fm.data[key] = val;
        }
    }
    return fm;
}

// ---------------------------------------------------------------------------
// Platform matching.
// ---------------------------------------------------------------------------

bool skill_matches_platform(const nlohmann::json& frontmatter) {
    if (!frontmatter.is_object()) return true;
    auto it = frontmatter.find("platforms");
    if (it == frontmatter.end()) return true;
    const auto& platforms = *it;
    if (platforms.is_null()) return true;
    if (platforms.is_array() && platforms.empty()) return true;

    auto current = current_platform_id();

    auto check_one = [&](const nlohmann::json& p) -> bool {
        std::string raw =
            p.is_string() ? p.get<std::string>() : p.dump();
        auto normalized = normalize_platform_name(raw);
        return starts_with(current, normalized);
    };

    if (platforms.is_array()) {
        for (const auto& p : platforms) {
            if (check_one(p)) return true;
        }
        return false;
    }
    return check_one(platforms);
}

// ---------------------------------------------------------------------------
// Disabled skills.
// ---------------------------------------------------------------------------

std::unordered_set<std::string> normalize_string_set(
    const nlohmann::json& values) {
    std::unordered_set<std::string> out;
    if (values.is_null()) return out;
    auto add_one = [&](const std::string& s) {
        auto t = strip(s);
        if (!t.empty()) out.insert(std::move(t));
    };
    if (values.is_string()) {
        add_one(values.get<std::string>());
    } else if (values.is_array()) {
        for (const auto& v : values) {
            if (v.is_string()) {
                add_one(v.get<std::string>());
            } else if (!v.is_null()) {
                add_one(v.dump());
            }
        }
    }
    return out;
}

std::unordered_set<std::string> get_disabled_skill_names(
    const std::string& platform) {
    auto config_path = hermes::core::path::get_hermes_home() / "config.yaml";
    std::error_code ec;
    if (!std::filesystem::exists(config_path, ec)) return {};
    auto text = read_file(config_path);
    if (text.empty()) return {};
    auto parsed_opt = parse_yaml(text);
    if (!parsed_opt || !parsed_opt->is_object()) return {};
    const auto& parsed = *parsed_opt;
    auto it = parsed.find("skills");
    if (it == parsed.end() || !it->is_object()) return {};

    auto resolved_platform = resolve_session_platform(platform);
    if (!resolved_platform.empty()) {
        auto pd = it->find("platform_disabled");
        if (pd != it->end() && pd->is_object()) {
            auto pe = pd->find(resolved_platform);
            if (pe != pd->end()) {
                return normalize_string_set(*pe);
            }
        }
    }
    auto d = it->find("disabled");
    if (d != it->end()) return normalize_string_set(*d);
    return {};
}

// ---------------------------------------------------------------------------
// External skills dirs.
// ---------------------------------------------------------------------------

std::string expand_path(const std::string& raw) {
    // Handle ~ prefix.
    std::string s = raw;
    if (!s.empty() && s[0] == '~') {
        const char* home = std::getenv("HOME");
        if (home) {
            s = std::string(home) + s.substr(1);
        }
    }
    // Expand ${VAR} references.  No default fallback: unset → empty.
    std::string out;
    out.reserve(s.size());
    for (std::size_t i = 0; i < s.size();) {
        if (s[i] == '$' && i + 1 < s.size() && s[i + 1] == '{') {
            auto close = s.find('}', i + 2);
            if (close != std::string::npos) {
                auto name = s.substr(i + 2, close - (i + 2));
                const char* val = std::getenv(name.c_str());
                if (val) out += val;
                i = close + 1;
                continue;
            }
        }
        out.push_back(s[i++]);
    }
    return out;
}

std::vector<std::filesystem::path> get_external_skills_dirs() {
    auto config_path = hermes::core::path::get_hermes_home() / "config.yaml";
    std::error_code ec;
    if (!std::filesystem::exists(config_path, ec)) return {};
    auto text = read_file(config_path);
    if (text.empty()) return {};
    auto parsed_opt = parse_yaml(text);
    if (!parsed_opt || !parsed_opt->is_object()) return {};
    auto skills_it = parsed_opt->find("skills");
    if (skills_it == parsed_opt->end() || !skills_it->is_object()) return {};
    auto raw_it = skills_it->find("external_dirs");
    if (raw_it == skills_it->end()) return {};

    std::vector<nlohmann::json> raw_list;
    if (raw_it->is_string()) {
        raw_list.push_back(*raw_it);
    } else if (raw_it->is_array()) {
        for (const auto& v : *raw_it) raw_list.push_back(v);
    } else {
        return {};
    }

    auto local_skills =
        std::filesystem::weakly_canonical(
            hermes::core::path::get_hermes_home() / "skills", ec);

    std::set<std::filesystem::path> seen;
    std::vector<std::filesystem::path> out;
    for (const auto& entry : raw_list) {
        if (!entry.is_string()) continue;
        auto raw = strip(entry.get<std::string>());
        if (raw.empty()) continue;
        auto expanded = expand_path(raw);
        auto p =
            std::filesystem::weakly_canonical(expanded, ec);
        if (!ec && p == local_skills) continue;
        if (seen.count(p)) continue;
        std::error_code ec2;
        if (std::filesystem::is_directory(p, ec2)) {
            seen.insert(p);
            out.push_back(p);
        }
    }
    return out;
}

std::vector<std::filesystem::path> get_all_skills_dirs() {
    std::vector<std::filesystem::path> out;
    out.push_back(hermes::core::path::get_hermes_home() / "skills");
    auto ext = get_external_skills_dirs();
    out.insert(out.end(), ext.begin(), ext.end());
    return out;
}

// ---------------------------------------------------------------------------
// Condition / config / description extraction.
// ---------------------------------------------------------------------------

namespace {

std::vector<std::string> json_to_string_list(const nlohmann::json& j) {
    std::vector<std::string> out;
    if (j.is_string()) {
        out.push_back(j.get<std::string>());
    } else if (j.is_array()) {
        for (const auto& v : j) {
            if (v.is_string()) out.push_back(v.get<std::string>());
            else if (!v.is_null()) out.push_back(v.dump());
        }
    }
    return out;
}

const nlohmann::json& empty_object() {
    static const nlohmann::json o = nlohmann::json::object();
    return o;
}

}  // namespace

SkillConditions extract_skill_conditions(const nlohmann::json& frontmatter) {
    SkillConditions c;
    if (!frontmatter.is_object()) return c;
    auto metadata_it = frontmatter.find("metadata");
    const nlohmann::json* metadata = &empty_object();
    if (metadata_it != frontmatter.end() && metadata_it->is_object()) {
        metadata = &*metadata_it;
    }
    auto hermes_it = metadata->find("hermes");
    const nlohmann::json* hermes = &empty_object();
    if (hermes_it != metadata->end() && hermes_it->is_object()) {
        hermes = &*hermes_it;
    }
    auto get_list = [&](const char* key) {
        auto it = hermes->find(key);
        return it == hermes->end() ? std::vector<std::string>{}
                                   : json_to_string_list(*it);
    };
    c.fallback_for_toolsets = get_list("fallback_for_toolsets");
    c.requires_toolsets = get_list("requires_toolsets");
    c.fallback_for_tools = get_list("fallback_for_tools");
    c.requires_tools = get_list("requires_tools");
    return c;
}

std::vector<SkillConfigVar> extract_skill_config_vars(
    const nlohmann::json& frontmatter) {
    std::vector<SkillConfigVar> out;
    if (!frontmatter.is_object()) return out;
    auto metadata_it = frontmatter.find("metadata");
    if (metadata_it == frontmatter.end() || !metadata_it->is_object())
        return out;
    auto hermes_it = metadata_it->find("hermes");
    if (hermes_it == metadata_it->end() || !hermes_it->is_object())
        return out;
    auto raw_it = hermes_it->find("config");
    if (raw_it == hermes_it->end()) return out;

    std::vector<nlohmann::json> items;
    if (raw_it->is_object()) {
        items.push_back(*raw_it);
    } else if (raw_it->is_array()) {
        for (const auto& v : *raw_it) items.push_back(v);
    } else {
        return out;
    }

    std::set<std::string> seen;
    for (const auto& item : items) {
        if (!item.is_object()) continue;
        std::string key;
        if (auto k = item.find("key"); k != item.end() && k->is_string()) {
            key = strip(k->get<std::string>());
        }
        if (key.empty() || seen.count(key)) continue;
        std::string desc;
        if (auto d = item.find("description");
            d != item.end() && d->is_string()) {
            desc = strip(d->get<std::string>());
        }
        if (desc.empty()) continue;
        SkillConfigVar v;
        v.key = key;
        v.description = desc;
        v.prompt = desc;
        if (auto d = item.find("default"); d != item.end() && !d->is_null()) {
            v.default_value = *d;
        }
        if (auto p = item.find("prompt");
            p != item.end() && p->is_string()) {
            auto ps = strip(p->get<std::string>());
            if (!ps.empty()) v.prompt = ps;
        }
        seen.insert(key);
        out.push_back(std::move(v));
    }
    return out;
}

std::vector<SkillConfigVar> discover_all_skill_config_vars() {
    std::vector<SkillConfigVar> out;
    std::set<std::string> seen_keys;

    auto disabled = get_disabled_skill_names();
    for (const auto& skills_dir : get_all_skills_dirs()) {
        std::error_code ec;
        if (!std::filesystem::is_directory(skills_dir, ec)) continue;
        for (const auto& skill_file :
             iter_skill_index_files(skills_dir, "SKILL.md")) {
            auto text = read_file(skill_file);
            if (text.empty()) continue;
            auto fm = parse_frontmatter(text);

            std::string skill_name;
            if (auto n = fm.data.find("name");
                n != fm.data.end() && n->is_string()) {
                skill_name = n->get<std::string>();
            } else {
                skill_name = skill_file.parent_path().filename().string();
            }
            if (disabled.count(skill_name)) continue;
            if (!skill_matches_platform(fm.data)) continue;

            for (auto& var : extract_skill_config_vars(fm.data)) {
                if (seen_keys.count(var.key)) continue;
                var.skill = skill_name;
                seen_keys.insert(var.key);
                out.push_back(std::move(var));
            }
        }
    }
    return out;
}

const nlohmann::json* resolve_dotpath(const nlohmann::json& config,
                                      const std::string& dotted_key) {
    std::vector<std::string> parts;
    std::size_t start = 0;
    while (true) {
        auto dot = dotted_key.find('.', start);
        if (dot == std::string::npos) {
            parts.push_back(dotted_key.substr(start));
            break;
        }
        parts.push_back(dotted_key.substr(start, dot - start));
        start = dot + 1;
    }
    const nlohmann::json* current = &config;
    for (const auto& part : parts) {
        if (!current->is_object()) return nullptr;
        auto it = current->find(part);
        if (it == current->end()) return nullptr;
        current = &*it;
    }
    return current;
}

nlohmann::json resolve_skill_config_values(
    const std::vector<SkillConfigVar>& config_vars) {
    auto config_path = hermes::core::path::get_hermes_home() / "config.yaml";
    nlohmann::json config = nlohmann::json::object();
    std::error_code ec;
    if (std::filesystem::exists(config_path, ec)) {
        auto text = read_file(config_path);
        if (!text.empty()) {
            auto parsed = parse_yaml(text);
            if (parsed && parsed->is_object()) config = *parsed;
        }
    }

    nlohmann::json out = nlohmann::json::object();
    for (const auto& var : config_vars) {
        std::string storage_key = std::string(kSkillConfigPrefix) + "." +
                                  var.key;
        const nlohmann::json* val = resolve_dotpath(config, storage_key);
        nlohmann::json value;
        bool empty_string =
            val && val->is_string() && strip(val->get<std::string>()).empty();
        if (!val || val->is_null() || empty_string) {
            value = var.default_value.value_or(nlohmann::json(""));
        } else {
            value = *val;
        }
        if (value.is_string()) {
            auto s = value.get<std::string>();
            if (s.find('~') != std::string::npos ||
                s.find("${") != std::string::npos) {
                value = expand_path(s);
            }
        }
        out[var.key] = value;
    }
    return out;
}

std::string extract_skill_description(const nlohmann::json& frontmatter) {
    if (!frontmatter.is_object()) return {};
    auto it = frontmatter.find("description");
    if (it == frontmatter.end()) return {};
    std::string raw;
    if (it->is_string()) {
        raw = it->get<std::string>();
    } else if (it->is_null()) {
        return {};
    } else {
        raw = it->dump();
    }
    auto trimmed = strip_quotes(strip(raw));
    if (trimmed.size() > 60) {
        return trimmed.substr(0, 57) + "...";
    }
    return trimmed;
}

// ---------------------------------------------------------------------------
// Directory walking.
// ---------------------------------------------------------------------------

std::vector<std::filesystem::path> iter_skill_index_files(
    const std::filesystem::path& skills_dir, const std::string& filename) {
    std::vector<std::filesystem::path> matches;
    std::error_code ec;
    if (!std::filesystem::is_directory(skills_dir, ec)) return matches;

    // Manual recursive walk so we can prune excluded directories.
    const auto& excluded = excluded_skill_dirs();
    std::vector<std::filesystem::path> stack;
    stack.push_back(skills_dir);
    while (!stack.empty()) {
        auto cur = stack.back();
        stack.pop_back();

        std::error_code it_ec;
        std::filesystem::directory_iterator it(cur, it_ec);
        if (it_ec) continue;
        std::filesystem::directory_iterator end;
        for (; it != end; ++it) {
            const auto& p = it->path();
            std::error_code e2;
            if (it->is_directory(e2)) {
                if (!excluded.count(p.filename().string())) {
                    stack.push_back(p);
                }
            } else if (it->is_regular_file(e2) &&
                       p.filename().string() == filename) {
                matches.push_back(p);
            }
        }
    }

    std::sort(matches.begin(), matches.end(),
              [&skills_dir](const auto& a, const auto& b) {
                  std::error_code e;
                  auto ra = std::filesystem::relative(a, skills_dir, e);
                  auto rb = std::filesystem::relative(b, skills_dir, e);
                  return ra < rb;
              });
    return matches;
}

}  // namespace hermes::agent::skill_utils
