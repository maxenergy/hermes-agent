#include "hermes/skills/skills_tool.hpp"

#include "hermes/skills/skill_utils.hpp"
#include "hermes/core/path.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <unordered_set>

namespace hermes::skills {

namespace fs = std::filesystem;

static const std::unordered_set<std::string>& excluded_components() {
    static const std::unordered_set<std::string> kExcluded{
        ".git", ".github", ".hub"};
    return kExcluded;
}

std::string to_string(SkillReadinessStatus s) {
    switch (s) {
        case SkillReadinessStatus::Available: return "available";
        case SkillReadinessStatus::SetupNeeded: return "setup_needed";
        case SkillReadinessStatus::Unsupported: return "unsupported";
    }
    return "available";
}

std::size_t estimate_tokens(std::string_view content) {
    return content.size() / 4;
}

namespace {

std::string trim(std::string s) {
    auto start = s.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return {};
    auto end = s.find_last_not_of(" \t\r\n");
    return s.substr(start, end - start + 1);
}

// Strip matching leading+trailing quotes.
std::string strip_quotes(std::string s) {
    if (s.size() >= 2 && ((s.front() == '"' && s.back() == '"') ||
                          (s.front() == '\'' && s.back() == '\''))) {
        return s.substr(1, s.size() - 2);
    }
    return s;
}

}  // namespace

std::vector<std::string> parse_tags(const nlohmann::json& tags_value) {
    std::vector<std::string> out;
    if (tags_value.is_null()) return out;
    if (tags_value.is_array()) {
        for (const auto& item : tags_value) {
            if (item.is_string()) {
                std::string t = trim(item.get<std::string>());
                if (!t.empty()) out.push_back(std::move(t));
            } else if (!item.is_null()) {
                std::string t = trim(item.dump());
                if (!t.empty()) out.push_back(std::move(t));
            }
        }
        return out;
    }
    if (tags_value.is_string()) {
        std::string v = trim(tags_value.get<std::string>());
        if (v.size() >= 2 && v.front() == '[' && v.back() == ']') {
            v = v.substr(1, v.size() - 2);
        }
        std::istringstream ss(v);
        std::string tok;
        while (std::getline(ss, tok, ',')) {
            tok = trim(tok);
            tok = strip_quotes(tok);
            if (!tok.empty()) out.push_back(std::move(tok));
        }
    }
    return out;
}

bool is_valid_env_var_name(std::string_view name) {
    if (name.empty()) return false;
    char c = name[0];
    if (!(std::isalpha(static_cast<unsigned char>(c)) || c == '_')) return false;
    for (char ch : name.substr(1)) {
        if (!(std::isalnum(static_cast<unsigned char>(ch)) || ch == '_')) return false;
    }
    return true;
}

std::string normalize_platform_token(std::string_view token) {
    std::string t(token);
    std::transform(t.begin(), t.end(), t.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    if (t == "macos" || t == "osx" || t == "mac") return "darwin";
    if (t == "windows") return "win32";
    if (t == "linux") return "linux";
    return t;
}

bool skill_matches_platform(const nlohmann::json& frontmatter,
                            std::string_view current_platform) {
    if (frontmatter.is_null() || !frontmatter.is_object()) return true;
    if (!frontmatter.contains("platforms")) return true;
    const auto& p = frontmatter["platforms"];
    std::vector<std::string> list;
    if (p.is_array()) {
        for (const auto& item : p) {
            if (item.is_string()) list.push_back(item.get<std::string>());
        }
    } else if (p.is_string()) {
        std::istringstream ss(p.get<std::string>());
        std::string tok;
        while (std::getline(ss, tok, ',')) {
            tok = trim(tok);
            if (!tok.empty()) list.push_back(std::move(tok));
        }
    } else {
        return true;
    }
    if (list.empty()) return true;
    std::string cp = std::string(current_platform);
    std::transform(cp.begin(), cp.end(), cp.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    for (const auto& raw : list) {
        std::string norm = normalize_platform_token(raw);
        if (!norm.empty() && cp.rfind(norm, 0) == 0) return true;
    }
    return false;
}

std::string truncate_description(std::string_view description) {
    if (description.size() <= kMaxSkillDescriptionLength) {
        return std::string(description);
    }
    return std::string(description.substr(0, kMaxSkillDescriptionLength - 3)) + "...";
}

static std::vector<std::string> normalize_str_list(const nlohmann::json& v) {
    std::vector<std::string> out;
    if (v.is_null()) return out;
    if (v.is_string()) {
        auto s = v.get<std::string>();
        if (!trim(s).empty()) out.push_back(std::move(s));
        return out;
    }
    if (v.is_array()) {
        for (const auto& item : v) {
            std::string s;
            if (item.is_string()) s = item.get<std::string>();
            else if (!item.is_null()) s = item.dump();
            if (!trim(s).empty()) out.push_back(std::move(s));
        }
    }
    return out;
}

Prerequisites collect_prerequisites(const nlohmann::json& frontmatter) {
    Prerequisites p;
    if (!frontmatter.is_object()) return p;
    if (!frontmatter.contains("prerequisites")) return p;
    const auto& pre = frontmatter["prerequisites"];
    if (!pre.is_object()) return p;
    if (pre.contains("env_vars")) p.env_vars = normalize_str_list(pre["env_vars"]);
    if (pre.contains("commands")) p.commands = normalize_str_list(pre["commands"]);
    return p;
}

SetupMetadata normalize_setup_metadata(const nlohmann::json& frontmatter) {
    SetupMetadata out;
    if (!frontmatter.is_object()) return out;
    if (!frontmatter.contains("setup")) return out;
    const auto& setup = frontmatter["setup"];
    if (!setup.is_object()) return out;

    if (setup.contains("help") && setup["help"].is_string()) {
        auto h = trim(setup["help"].get<std::string>());
        if (!h.empty()) out.help = std::move(h);
    }

    nlohmann::json raw = nlohmann::json::array();
    if (setup.contains("collect_secrets")) {
        if (setup["collect_secrets"].is_object()) {
            raw.push_back(setup["collect_secrets"]);
        } else if (setup["collect_secrets"].is_array()) {
            raw = setup["collect_secrets"];
        }
    }

    for (const auto& item : raw) {
        if (!item.is_object()) continue;
        CollectSecret cs;
        if (item.contains("env_var") && item["env_var"].is_string()) {
            cs.env_var = trim(item["env_var"].get<std::string>());
        }
        if (cs.env_var.empty()) continue;

        if (item.contains("prompt") && item["prompt"].is_string()) {
            cs.prompt = trim(item["prompt"].get<std::string>());
        }
        if (cs.prompt.empty()) {
            cs.prompt = "Enter value for " + cs.env_var;
        }

        if (item.contains("provider_url") && item["provider_url"].is_string()) {
            cs.provider_url = trim(item["provider_url"].get<std::string>());
        } else if (item.contains("url") && item["url"].is_string()) {
            cs.provider_url = trim(item["url"].get<std::string>());
        }

        cs.secret = true;
        if (item.contains("secret") && item["secret"].is_boolean()) {
            cs.secret = item["secret"].get<bool>();
        }

        out.collect_secrets.push_back(std::move(cs));
    }
    return out;
}

std::vector<RequiredEnvVar> get_required_environment_variables(
    const nlohmann::json& frontmatter) {
    std::vector<RequiredEnvVar> out;
    std::unordered_set<std::string> seen;

    auto setup = normalize_setup_metadata(frontmatter);

    auto append = [&](const std::string& name,
                      const std::string& prompt,
                      const std::string& help,
                      const std::string& required_for) {
        if (name.empty() || seen.count(name)) return;
        if (!is_valid_env_var_name(name)) return;
        RequiredEnvVar ev;
        ev.name = name;
        ev.prompt = prompt.empty() ? ("Enter value for " + name) : prompt;
        ev.help = help;
        ev.required_for = required_for;
        seen.insert(name);
        out.push_back(std::move(ev));
    };

    if (frontmatter.is_object() &&
        frontmatter.contains("required_environment_variables")) {
        nlohmann::json raw = frontmatter["required_environment_variables"];
        if (raw.is_object()) {
            auto tmp = nlohmann::json::array();
            tmp.push_back(raw);
            raw = tmp;
        }
        if (raw.is_array()) {
            for (const auto& item : raw) {
                if (item.is_string()) {
                    append(item.get<std::string>(), "", "", "");
                } else if (item.is_object()) {
                    std::string name;
                    if (item.contains("name") && item["name"].is_string()) {
                        name = trim(item["name"].get<std::string>());
                    } else if (item.contains("env_var") &&
                               item["env_var"].is_string()) {
                        name = trim(item["env_var"].get<std::string>());
                    }
                    std::string prompt;
                    if (item.contains("prompt") && item["prompt"].is_string()) {
                        prompt = trim(item["prompt"].get<std::string>());
                    }
                    std::string help;
                    for (const char* key : {"help", "provider_url", "url"}) {
                        if (item.contains(key) && item[key].is_string()) {
                            help = trim(item[key].get<std::string>());
                            if (!help.empty()) break;
                        }
                    }
                    if (help.empty() && setup.help) help = *setup.help;
                    std::string req_for;
                    if (item.contains("required_for") &&
                        item["required_for"].is_string()) {
                        req_for = trim(item["required_for"].get<std::string>());
                    }
                    append(name, prompt, help, req_for);
                }
            }
        }
    }

    for (const auto& cs : setup.collect_secrets) {
        std::string help = cs.provider_url;
        if (help.empty() && setup.help) help = *setup.help;
        append(cs.env_var, cs.prompt, help, "");
    }

    Prerequisites pre = collect_prerequisites(frontmatter);
    for (const auto& name : pre.env_vars) {
        append(name, "", "", "");
    }
    return out;
}

std::string gateway_setup_hint() {
    return "Secure secret entry is not available. Load this skill in the local "
           "CLI to be prompted, or add the key to ~/.hermes/.env manually.";
}

std::optional<std::string> build_setup_note(
    SkillReadinessStatus status,
    const std::vector<std::string>& missing,
    std::string_view setup_help) {
    if (status != SkillReadinessStatus::SetupNeeded) return std::nullopt;
    std::string list;
    if (missing.empty()) {
        list = "required prerequisites";
    } else {
        for (std::size_t i = 0; i < missing.size(); ++i) {
            if (i) list += ", ";
            list += missing[i];
        }
    }
    std::string note = "Setup needed before using this skill: missing " + list + ".";
    if (!setup_help.empty()) {
        note.push_back(' ');
        note.append(setup_help);
    }
    return note;
}

std::unordered_map<std::string, std::string> parse_dotenv(std::string_view body) {
    std::unordered_map<std::string, std::string> out;
    std::string buf(body);
    std::istringstream ss(buf);
    std::string line;
    while (std::getline(ss, line)) {
        std::string l = trim(line);
        if (l.empty() || l.front() == '#') continue;
        auto eq = l.find('=');
        if (eq == std::string::npos) continue;
        std::string key = trim(l.substr(0, eq));
        std::string value = trim(l.substr(eq + 1));
        value = strip_quotes(value);
        if (!key.empty()) out[key] = value;
    }
    return out;
}

std::unordered_map<std::string, std::string> load_env_file() {
    auto path = hermes::core::path::get_hermes_home() / ".env";
    std::error_code ec;
    if (!fs::is_regular_file(path, ec)) return {};
    std::ifstream ifs(path);
    if (!ifs) return {};
    std::string body((std::istreambuf_iterator<char>(ifs)),
                     std::istreambuf_iterator<char>());
    return parse_dotenv(body);
}

bool is_env_var_persisted(
    const std::string& name,
    const std::unordered_map<std::string, std::string>& snapshot) {
    auto it = snapshot.find(name);
    if (it != snapshot.end()) return !it->second.empty();
    const char* env = std::getenv(name.c_str());
    return env != nullptr && env[0] != '\0';
}

std::vector<std::string> remaining_required_environment_names(
    const std::vector<RequiredEnvVar>& required,
    const std::vector<std::string>& capture_missing,
    const std::unordered_map<std::string, std::string>& snapshot) {
    std::unordered_set<std::string> missing_set(
        capture_missing.begin(), capture_missing.end());
    std::vector<std::string> out;
    for (const auto& entry : required) {
        if (missing_set.count(entry.name) ||
            !is_env_var_persisted(entry.name, snapshot)) {
            out.push_back(entry.name);
        }
    }
    return out;
}

ReadinessReport classify_readiness(
    const nlohmann::json& frontmatter,
    const std::unordered_map<std::string, std::string>& env_snapshot) {
    ReadinessReport rep;
    auto required = get_required_environment_variables(frontmatter);
    rep.missing_env_vars = remaining_required_environment_names(
        required, /*capture_missing=*/{}, env_snapshot);

    auto setup = normalize_setup_metadata(frontmatter);
    if (setup.help) rep.setup_help = *setup.help;

    if (!rep.missing_env_vars.empty()) {
        rep.status = SkillReadinessStatus::SetupNeeded;
        rep.note = build_setup_note(
            rep.status, rep.missing_env_vars,
            rep.setup_help ? std::string_view(*rep.setup_help) : std::string_view{});
    } else {
        rep.status = SkillReadinessStatus::Available;
    }
    return rep;
}

std::string category_from_path(const fs::path& skill_md,
                               const std::vector<fs::path>& skills_dirs) {
    for (const auto& root : skills_dirs) {
        std::error_code ec;
        auto rel = fs::relative(skill_md, root, ec);
        if (ec) continue;
        const auto& rel_str = rel.generic_string();
        if (rel_str.find("..") == 0) continue;  // not under root
        // parts = dirs + filename. Expect: <cat>/<name>/SKILL.md (3+ parts).
        std::vector<std::string> parts;
        for (const auto& part : rel) parts.push_back(part.string());
        if (parts.size() >= 3) return parts.front();
    }
    return {};
}

std::optional<std::string> load_category_description(
    const fs::path& category_dir) {
    auto desc = category_dir / "DESCRIPTION.md";
    std::error_code ec;
    if (!fs::is_regular_file(desc, ec)) return std::nullopt;
    std::ifstream ifs(desc);
    if (!ifs) return std::nullopt;
    std::string content((std::istreambuf_iterator<char>(ifs)),
                         std::istreambuf_iterator<char>());
    auto [meta, body] = parse_frontmatter(content);
    std::string description;
    if (meta.is_object() && meta.contains("description") &&
        meta["description"].is_string()) {
        description = meta["description"].get<std::string>();
    }
    if (description.empty()) {
        std::istringstream ss(body);
        std::string line;
        while (std::getline(ss, line)) {
            std::string tl = trim(line);
            if (tl.empty()) continue;
            if (tl.front() == '#') continue;
            description = tl;
            break;
        }
    }
    if (description.empty()) return std::nullopt;
    return truncate_description(description);
}

namespace {

std::string current_platform() {
#if defined(__APPLE__)
    return "darwin";
#elif defined(_WIN32)
    return "win32";
#else
    return "linux";
#endif
}

bool has_excluded_component(const fs::path& p) {
    const auto& exc = excluded_components();
    for (const auto& part : p) {
        if (exc.count(part.string())) return true;
    }
    return false;
}

std::string read_head(const fs::path& file, std::size_t max_bytes) {
    std::ifstream ifs(file, std::ios::binary);
    if (!ifs) return {};
    std::string buf;
    buf.resize(max_bytes);
    ifs.read(buf.data(), static_cast<std::streamsize>(buf.size()));
    buf.resize(static_cast<std::size_t>(ifs.gcount()));
    return buf;
}

}  // namespace

std::vector<SkillListEntry> find_skills_in_dir(
    const fs::path& scan_dir,
    std::string_view current_platform_in) {
    std::vector<SkillListEntry> out;
    std::error_code ec;
    if (!fs::is_directory(scan_dir, ec)) return out;

    std::string platform = current_platform_in.empty()
                               ? current_platform()
                               : std::string(current_platform_in);

    std::vector<fs::path> scan_dirs{scan_dir};

    for (auto it = fs::recursive_directory_iterator(scan_dir, ec);
         it != fs::recursive_directory_iterator();) {
        const auto& entry = *it;
        if (entry.is_regular_file(ec) &&
            entry.path().filename() == "SKILL.md" &&
            !has_excluded_component(entry.path())) {
            std::string content = read_head(entry.path(), 4000);
            auto [meta, body] = parse_frontmatter(content);
            if (!skill_matches_platform(meta, platform)) {
                it.increment(ec);
                continue;
            }
            SkillListEntry e;
            e.path = entry.path().parent_path();
            if (meta.is_object() && meta.contains("name") &&
                meta["name"].is_string()) {
                e.name = meta["name"].get<std::string>();
            } else {
                e.name = entry.path().parent_path().filename().string();
            }
            if (e.name.size() > kMaxSkillNameLength) {
                e.name = e.name.substr(0, kMaxSkillNameLength);
            }

            if (meta.is_object() && meta.contains("description") &&
                meta["description"].is_string()) {
                e.description = meta["description"].get<std::string>();
            }
            if (e.description.empty()) {
                std::istringstream ss(body);
                std::string line;
                while (std::getline(ss, line)) {
                    std::string tl = trim(line);
                    if (tl.empty()) continue;
                    if (tl.front() == '#') continue;
                    e.description = tl;
                    break;
                }
            }
            e.description = truncate_description(e.description);
            e.category = category_from_path(entry.path(), scan_dirs);
            out.push_back(std::move(e));
        }
        it.increment(ec);
        if (ec) break;
    }
    return out;
}

std::vector<SkillListEntry> merge_skill_lists(
    std::vector<std::vector<SkillListEntry>> dir_results,
    const std::vector<std::string>& disabled_names) {
    std::unordered_set<std::string> disabled(
        disabled_names.begin(), disabled_names.end());
    std::unordered_set<std::string> seen;
    std::vector<SkillListEntry> out;
    for (auto& group : dir_results) {
        for (auto& entry : group) {
            if (disabled.count(entry.name)) continue;
            if (seen.count(entry.name)) continue;
            seen.insert(entry.name);
            out.push_back(std::move(entry));
        }
    }
    return out;
}

nlohmann::json build_skill_view_envelope(std::string_view skill_name,
                                          std::string_view skill_md_content) {
    auto [meta, body] = parse_frontmatter(skill_md_content);
    nlohmann::json out;
    out["name"] = std::string(skill_name);
    out["description"] =
        (meta.is_object() && meta.contains("description") &&
         meta["description"].is_string())
            ? meta["description"].get<std::string>()
            : std::string{};
    out["body"] = body;
    out["tokens"] = estimate_tokens(skill_md_content);
    if (meta.is_object()) {
        out["frontmatter"] = meta;
    } else {
        out["frontmatter"] = nlohmann::json::object();
    }
    return out;
}

}  // namespace hermes::skills
