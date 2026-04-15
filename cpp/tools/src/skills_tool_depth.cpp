// Implementation of hermes/tools/skills_tool_depth.hpp.
#include "hermes/tools/skills_tool_depth.hpp"

#include <algorithm>
#include <cctype>
#include <regex>
#include <sstream>
#include <string>
#include <unordered_set>

namespace hermes::tools::skills_depth {

namespace {

std::string to_lower(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        out.push_back(static_cast<char>(
            std::tolower(static_cast<unsigned char>(c))));
    }
    return out;
}

std::string trim_ascii(std::string_view s) {
    std::size_t b{0};
    std::size_t e{s.size()};
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) {
        ++b;
    }
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) {
        --e;
    }
    return std::string{s.substr(b, e - b)};
}

std::string json_to_string(const nlohmann::json& v) {
    if (v.is_string()) {
        return v.get<std::string>();
    }
    if (v.is_number_integer()) {
        return std::to_string(v.get<long long>());
    }
    if (v.is_number_unsigned()) {
        return std::to_string(v.get<unsigned long long>());
    }
    if (v.is_number_float()) {
        std::ostringstream os;
        os << v.get<double>();
        return os.str();
    }
    if (v.is_boolean()) {
        return v.get<bool>() ? "true" : "false";
    }
    return {};
}

std::vector<std::string> split_by_char(std::string_view s, char c) {
    std::vector<std::string> out{};
    std::string acc{};
    for (char ch : s) {
        if (ch == c) {
            out.push_back(acc);
            acc.clear();
        } else {
            acc.push_back(ch);
        }
    }
    out.push_back(acc);
    return out;
}

}  // namespace

// ---- Readiness ---------------------------------------------------------

ReadinessStatus parse_readiness(std::string_view raw) {
    std::string t{to_lower(trim_ascii(raw))};
    if (t == "ready") return ReadinessStatus::Ready;
    if (t == "missing_env" || t == "missing-env") {
        return ReadinessStatus::MissingEnv;
    }
    if (t == "missing_command" || t == "missing-command") {
        return ReadinessStatus::MissingCommand;
    }
    if (t == "incompatible") return ReadinessStatus::Incompatible;
    return ReadinessStatus::Unknown;
}

std::string readiness_name(ReadinessStatus status) {
    switch (status) {
        case ReadinessStatus::Ready: return "ready";
        case ReadinessStatus::MissingEnv: return "missing_env";
        case ReadinessStatus::MissingCommand: return "missing_command";
        case ReadinessStatus::Incompatible: return "incompatible";
        case ReadinessStatus::Unknown:
        default: return "unknown";
    }
}

// ---- Prerequisites -----------------------------------------------------

std::vector<std::string> normalise_prerequisite_values(
    const nlohmann::json& value) {
    std::vector<std::string> out{};
    if (value.is_null()) {
        return out;
    }
    if (value.is_string()) {
        std::string s{trim_ascii(value.get<std::string>())};
        if (!s.empty()) {
            out.push_back(value.get<std::string>());
        }
        return out;
    }
    if (value.is_array()) {
        for (const auto& item : value) {
            std::string s{json_to_string(item)};
            if (!trim_ascii(s).empty()) {
                out.push_back(s);
            }
        }
    }
    return out;
}

PrereqLists collect_prerequisite_values(const nlohmann::json& frontmatter) {
    PrereqLists out{};
    if (!frontmatter.is_object()) {
        return out;
    }
    auto it = frontmatter.find("prerequisites");
    if (it == frontmatter.end() || !it->is_object()) {
        return out;
    }
    if (auto env_it = it->find("env_vars"); env_it != it->end()) {
        out.env_vars = normalise_prerequisite_values(*env_it);
    }
    if (auto cmd_it = it->find("commands"); cmd_it != it->end()) {
        out.commands = normalise_prerequisite_values(*cmd_it);
    }
    return out;
}

SetupMetadata normalise_setup_metadata(const nlohmann::json& frontmatter) {
    SetupMetadata out{};
    if (!frontmatter.is_object()) {
        return out;
    }
    auto it = frontmatter.find("setup");
    if (it == frontmatter.end() || !it->is_object()) {
        return out;
    }
    // help
    if (auto h = it->find("help"); h != it->end() && h->is_string()) {
        std::string trimmed{trim_ascii(h->get<std::string>())};
        if (!trimmed.empty()) {
            out.help = trimmed;
        }
    }
    // collect_secrets may be a dict or list
    nlohmann::json raw_secrets = nlohmann::json::array();
    if (auto cs = it->find("collect_secrets"); cs != it->end()) {
        if (cs->is_object()) {
            raw_secrets.push_back(*cs);
        } else if (cs->is_array()) {
            raw_secrets = *cs;
        }
    }
    for (const auto& item : raw_secrets) {
        if (!item.is_object()) continue;
        std::string env_var{};
        if (auto e = item.find("env_var"); e != item.end() && e->is_string()) {
            env_var = trim_ascii(e->get<std::string>());
        }
        if (env_var.empty()) continue;

        std::string prompt{};
        if (auto p = item.find("prompt"); p != item.end() && p->is_string()) {
            prompt = trim_ascii(p->get<std::string>());
        }
        if (prompt.empty()) {
            prompt = "Enter value for " + env_var;
        }
        std::string provider_url{};
        if (auto p = item.find("provider_url");
            p != item.end() && p->is_string()) {
            provider_url = trim_ascii(p->get<std::string>());
        } else if (auto u = item.find("url");
                   u != item.end() && u->is_string()) {
            provider_url = trim_ascii(u->get<std::string>());
        }
        bool secret{true};
        if (auto s = item.find("secret");
            s != item.end() && s->is_boolean()) {
            secret = s->get<bool>();
        }
        out.collect_secrets.push_back(
            CollectSecret{env_var, prompt, secret, provider_url});
    }
    return out;
}

bool is_valid_env_var_name(std::string_view name) {
    if (name.empty()) return false;
    char first{name[0]};
    if (!(first == '_' ||
          (first >= 'A' && first <= 'Z') ||
          (first >= 'a' && first <= 'z'))) {
        return false;
    }
    for (std::size_t i{1}; i < name.size(); ++i) {
        char c{name[i]};
        bool ok{c == '_' ||
                (c >= 'A' && c <= 'Z') ||
                (c >= 'a' && c <= 'z') ||
                (c >= '0' && c <= '9')};
        if (!ok) return false;
    }
    return true;
}

std::vector<RequiredEnvEntry> get_required_environment_variables(
    const nlohmann::json& frontmatter,
    const std::optional<std::vector<std::string>>& legacy_env_vars) {
    std::vector<RequiredEnvEntry> out{};
    std::unordered_set<std::string> seen{};

    SetupMetadata setup{normalise_setup_metadata(frontmatter)};

    auto append = [&](const std::string& env_name,
                      const std::string& prompt_in,
                      const std::string& help_in,
                      const std::string& required_for) {
        std::string name{trim_ascii(env_name)};
        if (name.empty() || seen.count(name) != 0u) return;
        if (!is_valid_env_var_name(name)) return;
        RequiredEnvEntry entry{};
        entry.name = name;
        std::string pr{trim_ascii(prompt_in)};
        entry.prompt = pr.empty() ? ("Enter value for " + name) : pr;
        std::string help{trim_ascii(help_in)};
        if (help.empty() && setup.help.has_value()) {
            help = *setup.help;
        }
        entry.help = help;
        entry.required_for = trim_ascii(required_for);
        seen.insert(name);
        out.push_back(std::move(entry));
    };

    // New structured list first.
    nlohmann::json required_raw = nlohmann::json::array();
    if (frontmatter.is_object()) {
        if (auto r = frontmatter.find("required_environment_variables");
            r != frontmatter.end()) {
            if (r->is_object()) {
                required_raw.push_back(*r);
            } else if (r->is_array()) {
                required_raw = *r;
            }
        }
    }
    for (const auto& item : required_raw) {
        if (item.is_string()) {
            append(item.get<std::string>(), "", "", "");
            continue;
        }
        if (!item.is_object()) continue;
        std::string name{};
        if (auto n = item.find("name"); n != item.end() && n->is_string()) {
            name = n->get<std::string>();
        } else if (auto e = item.find("env_var");
                   e != item.end() && e->is_string()) {
            name = e->get<std::string>();
        }
        std::string prompt{};
        if (auto p = item.find("prompt"); p != item.end() && p->is_string()) {
            prompt = p->get<std::string>();
        }
        std::string help{};
        if (auto h = item.find("help"); h != item.end() && h->is_string()) {
            help = h->get<std::string>();
        } else if (auto u = item.find("provider_url");
                   u != item.end() && u->is_string()) {
            help = u->get<std::string>();
        } else if (auto u2 = item.find("url");
                   u2 != item.end() && u2->is_string()) {
            help = u2->get<std::string>();
        }
        std::string required_for{};
        if (auto rf = item.find("required_for");
            rf != item.end() && rf->is_string()) {
            required_for = rf->get<std::string>();
        }
        append(name, prompt, help, required_for);
    }

    // Then setup.collect_secrets.
    for (const auto& cs : setup.collect_secrets) {
        std::string help{cs.provider_url.empty() && setup.help.has_value()
                             ? *setup.help
                             : cs.provider_url};
        append(cs.env_var, cs.prompt, help, "");
    }

    // Finally legacy prerequisites.env_vars.
    std::vector<std::string> legacy{};
    if (legacy_env_vars.has_value()) {
        legacy = *legacy_env_vars;
    } else {
        legacy = collect_prerequisite_values(frontmatter).env_vars;
    }
    for (const auto& v : legacy) {
        append(v, "", "", "");
    }

    return out;
}

// ---- Lightweight parsing ------------------------------------------------

std::size_t estimate_tokens(std::string_view content) {
    return content.size() / 4u;
}

std::vector<std::string> parse_tags(const nlohmann::json& raw) {
    std::vector<std::string> out{};
    if (raw.is_null()) return out;
    if (raw.is_array()) {
        for (const auto& t : raw) {
            std::string s{json_to_string(t)};
            std::string trimmed{trim_ascii(s)};
            if (!trimmed.empty()) {
                out.push_back(trimmed);
            }
        }
        return out;
    }
    std::string s{json_to_string(raw)};
    s = trim_ascii(s);
    if (s.empty()) return out;
    if (s.size() >= 2 && s.front() == '[' && s.back() == ']') {
        s = s.substr(1, s.size() - 2);
    }
    for (auto& piece : split_by_char(s, ',')) {
        std::string trimmed{trim_ascii(piece)};
        // Strip surrounding ASCII quotes.
        while (!trimmed.empty() &&
               (trimmed.front() == '"' || trimmed.front() == '\'')) {
            trimmed.erase(trimmed.begin());
        }
        while (!trimmed.empty() &&
               (trimmed.back() == '"' || trimmed.back() == '\'')) {
            trimmed.pop_back();
        }
        if (!trimmed.empty()) {
            out.push_back(trimmed);
        }
    }
    return out;
}

std::string category_from_relative_path(std::string_view rel_path) {
    auto parts = split_by_char(rel_path, '/');
    // Filter empty segments (e.g. leading slash).
    std::vector<std::string> segs{};
    for (auto& p : parts) {
        if (!p.empty()) segs.push_back(p);
    }
    if (segs.size() >= 3u) {
        return segs.front();
    }
    return {};
}

// ---- Disabled-skill detection ------------------------------------------

std::string normalise_platform_id(std::string_view raw) {
    return to_lower(trim_ascii(raw));
}

bool is_skill_disabled(std::string_view name,
                       const std::vector<std::string>& disabled_all,
                       const std::vector<std::string>& disabled_on_platform) {
    std::string lname{to_lower(name)};
    auto match = [&](const std::vector<std::string>& xs) {
        for (const auto& x : xs) {
            if (to_lower(x) == lname) return true;
        }
        return false;
    };
    return match(disabled_all) || match(disabled_on_platform);
}

// ---- Gateway surface detection -----------------------------------------

const std::vector<std::string>& gateway_surface_names() {
    static const std::vector<std::string> k{
        "telegram", "discord", "slack",  "signal",
        "matrix",   "feishu",  "weixin", "whatsapp",
    };
    return k;
}

bool is_gateway_surface(std::string_view platform) {
    std::string p{normalise_platform_id(platform)};
    for (const auto& s : gateway_surface_names()) {
        if (s == p) return true;
    }
    return false;
}

std::string format_setup_hint(const std::vector<std::string>& missing,
                              bool on_gateway) {
    if (missing.empty()) return {};
    std::ostringstream os;
    if (on_gateway) {
        os << "Ask an admin to run `/setup` from a Hermes admin chat and add: ";
    } else {
        os << "Add these env vars to ~/.hermes/.env: ";
    }
    bool first{true};
    for (const auto& name : missing) {
        if (!first) os << ", ";
        first = false;
        os << name;
    }
    return os.str();
}

std::vector<std::string> remaining_required_env_names(
    const std::vector<RequiredEnvEntry>& required,
    EnvPersistedFn is_persisted) {
    std::vector<std::string> out{};
    for (const auto& entry : required) {
        bool persisted{is_persisted != nullptr &&
                       is_persisted(entry.name)};
        if (!persisted) {
            out.push_back(entry.name);
        }
    }
    return out;
}

// ---- Listing -----------------------------------------------------------

nlohmann::json render_skills_list(
    const std::vector<SkillBriefEntry>& entries) {
    nlohmann::json out;
    out["count"] = entries.size();
    nlohmann::json arr = nlohmann::json::array();
    for (const auto& e : entries) {
        nlohmann::json item;
        item["name"] = e.name;
        item["description"] = e.description;
        item["category"] = e.category;
        item["tags"] = e.tags;
        item["tokens"] = e.tokens;
        arr.push_back(std::move(item));
    }
    out["skills"] = std::move(arr);
    return out;
}

std::vector<SkillBriefEntry> filter_by_category(
    const std::vector<SkillBriefEntry>& entries, std::string_view category) {
    std::string target{to_lower(trim_ascii(category))};
    if (target.empty()) return entries;
    std::vector<SkillBriefEntry> out{};
    for (const auto& e : entries) {
        if (to_lower(e.category) == target) {
            out.push_back(e);
        }
    }
    return out;
}

nlohmann::json group_by_category(
    const std::vector<SkillBriefEntry>& entries) {
    nlohmann::json out = nlohmann::json::object();
    std::vector<std::string> order{};
    for (const auto& e : entries) {
        std::string cat{e.category.empty() ? "misc" : e.category};
        if (!out.contains(cat)) {
            out[cat] = nlohmann::json::array();
            order.push_back(cat);
        }
        nlohmann::json item;
        item["name"] = e.name;
        item["description"] = e.description;
        out[cat].push_back(std::move(item));
    }
    nlohmann::json wrapped;
    wrapped["categories"] = order;
    wrapped["by_category"] = out;
    wrapped["total"] = entries.size();
    return wrapped;
}

// ---- Frontmatter split --------------------------------------------------

FrontmatterSplit split_frontmatter(std::string_view content) {
    FrontmatterSplit out{};
    std::string_view body{content};
    // Skip a UTF-8 BOM if present.
    static const char bom[] = "\xEF\xBB\xBF";
    if (body.size() >= 3 &&
        body.substr(0, 3) == std::string_view{bom, 3}) {
        body.remove_prefix(3);
    }
    if (body.size() < 3 || body.substr(0, 3) != "---") {
        return out;
    }
    body.remove_prefix(3);
    // Require a newline after the opening fence.
    if (body.empty() || (body.front() != '\n' && body.front() != '\r')) {
        return out;
    }
    if (body.front() == '\r' && body.size() >= 2 && body[1] == '\n') {
        body.remove_prefix(2);
    } else {
        body.remove_prefix(1);
    }
    // Look for "\n---" fence end.
    std::string pattern{"\n---"};
    auto end = body.find(pattern);
    if (end == std::string_view::npos) {
        return out;
    }
    out.yaml_block = std::string{body.substr(0, end)};
    auto after = end + pattern.size();
    // Skip optional trailing whitespace + newline on the closing fence.
    while (after < body.size() &&
           (body[after] == ' ' || body[after] == '\t')) {
        ++after;
    }
    if (after < body.size() && body[after] == '\r') ++after;
    if (after < body.size() && body[after] == '\n') ++after;
    out.body = std::string{body.substr(after)};
    out.ok = true;
    return out;
}

}  // namespace hermes::tools::skills_depth
