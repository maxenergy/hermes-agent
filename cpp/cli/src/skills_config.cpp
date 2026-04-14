// skills_config — implementation. See skills_config.hpp.
#include "hermes/cli/skills_config.hpp"
#include "hermes/config/loader.hpp"
#include "hermes/skills/skill_utils.hpp"

#include <algorithm>
#include <iostream>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace hermes::cli::skills_config {

const std::vector<std::pair<std::string, std::string>>& platform_labels() {
    static const std::vector<std::pair<std::string, std::string>> kLabels = {
        {"cli",          "CLI"},
        {"telegram",     "Telegram"},
        {"discord",      "Discord"},
        {"slack",        "Slack"},
        {"whatsapp",     "WhatsApp"},
        {"signal",       "Signal"},
        {"bluebubbles",  "BlueBubbles"},
        {"email",        "Email"},
        {"homeassistant","Home Assistant"},
        {"mattermost",   "Mattermost"},
        {"matrix",       "Matrix"},
        {"dingtalk",     "DingTalk"},
        {"feishu",       "Feishu"},
        {"wecom",        "WeCom"},
        {"weixin",       "Weixin"},
        {"webhook",      "Webhook"},
    };
    return kLabels;
}

std::string platform_label(const std::string& platform) {
    if (platform.empty()) return "All platforms";
    for (const auto& [key, label] : platform_labels()) {
        if (key == platform) return label;
    }
    return "All platforms";
}

std::set<std::string> get_disabled_skills(const nlohmann::json& config,
                                          const std::string& platform) {
    std::set<std::string> out;
    if (!config.is_object() || !config.contains("skills")) return out;
    const auto& skills = config["skills"];
    if (!skills.is_object()) return out;

    auto read_array = [&out](const nlohmann::json& arr) {
        if (!arr.is_array()) return;
        for (const auto& v : arr) {
            if (v.is_string()) out.insert(v.get<std::string>());
        }
    };

    if (platform.empty()) {
        if (skills.contains("disabled")) read_array(skills["disabled"]);
        return out;
    }
    if (skills.contains("platform_disabled")) {
        const auto& pd = skills["platform_disabled"];
        if (pd.is_object() && pd.contains(platform)) {
            read_array(pd[platform]);
            return out;
        }
    }
    // Fallback to global disabled list.
    if (skills.contains("disabled")) read_array(skills["disabled"]);
    return out;
}

void save_disabled_skills(nlohmann::json& config,
                          const std::set<std::string>& disabled,
                          const std::string& platform) {
    if (!config.is_object()) config = nlohmann::json::object();
    if (!config.contains("skills") || !config["skills"].is_object()) {
        config["skills"] = nlohmann::json::object();
    }
    nlohmann::json arr = nlohmann::json::array();
    for (const auto& s : disabled) arr.push_back(s);
    if (platform.empty()) {
        config["skills"]["disabled"] = arr;
    } else {
        if (!config["skills"].contains("platform_disabled") ||
            !config["skills"]["platform_disabled"].is_object()) {
            config["skills"]["platform_disabled"] = nlohmann::json::object();
        }
        config["skills"]["platform_disabled"][platform] = arr;
    }
}

std::vector<SkillInfo> load_skills() {
    std::vector<SkillInfo> out;
    for (const auto& s : hermes::skills::iter_skill_index()) {
        SkillInfo info;
        info.name = s.name;
        info.description = s.description;
        info.category = s.categories.empty() ? "uncategorized"
                                             : s.categories.front();
        out.push_back(std::move(info));
    }
    return out;
}

std::vector<std::string> categories(const std::vector<SkillInfo>& skills) {
    std::set<std::string> unique;
    for (const auto& s : skills) {
        unique.insert(s.category.empty() ? "uncategorized" : s.category);
    }
    return {unique.begin(), unique.end()};
}

std::vector<std::pair<std::string, std::vector<std::string>>>
group_by_category(const std::vector<SkillInfo>& skills) {
    std::vector<std::pair<std::string, std::vector<std::string>>> out;
    auto find_or_add = [&out](const std::string& cat) -> std::vector<std::string>& {
        for (auto& p : out) if (p.first == cat) return p.second;
        out.emplace_back(cat, std::vector<std::string>{});
        return out.back().second;
    };
    for (const auto& s : skills) {
        find_or_add(s.category.empty() ? "uncategorized" : s.category).push_back(s.name);
    }
    std::sort(out.begin(), out.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });
    for (auto& p : out) std::sort(p.second.begin(), p.second.end());
    return out;
}

std::set<std::string> toggle_category(
    const std::vector<SkillInfo>& skills,
    const std::set<std::string>& disabled,
    const std::string& category,
    bool enable) {
    std::set<std::string> out = disabled;
    for (const auto& s : skills) {
        const std::string c = s.category.empty() ? "uncategorized" : s.category;
        if (c != category) continue;
        if (enable) out.erase(s.name);
        else out.insert(s.name);
    }
    return out;
}

std::set<std::string> apply_individual_toggle(
    const std::vector<SkillInfo>& skills,
    const std::set<std::size_t>& chosen) {
    std::set<std::string> disabled;
    for (std::size_t i = 0; i < skills.size(); ++i) {
        if (!chosen.count(i)) disabled.insert(skills[i].name);
    }
    return disabled;
}

std::string format_row(const SkillInfo& skill, std::size_t desc_len) {
    std::string desc = skill.description;
    if (desc.size() > desc_len) desc = desc.substr(0, desc_len);
    std::string cat = skill.category.empty() ? "uncategorized" : skill.category;
    return skill.name + "  (" + cat + ")  —  " + desc;
}

Summary summarise(const std::vector<SkillInfo>& skills,
                  const std::set<std::string>& disabled) {
    Summary s;
    for (const auto& sk : skills) {
        if (disabled.count(sk.name)) ++s.disabled;
        else ++s.enabled;
    }
    return s;
}

std::string format_save_message(const Summary& s,
                                const std::string& platform_label_) {
    std::ostringstream oss;
    oss << "\u2713 Saved: " << s.enabled << " enabled, "
        << s.disabled << " disabled (" << platform_label_ << ").";
    return oss.str();
}

int cmd_show(const nlohmann::json& config,
             const std::string& platform,
             std::ostream& out) {
    auto disabled = get_disabled_skills(config, platform);
    out << "Skills disabled for " << platform_label(platform) << ": ";
    if (disabled.empty()) {
        out << "(none)\n";
        return 0;
    }
    bool first = true;
    for (const auto& s : disabled) {
        if (!first) out << ", ";
        out << s;
        first = false;
    }
    out << "\n";
    return 0;
}

int dispatch(int argc, char** argv) {
    // hermes skills         -> list
    // hermes skills show [platform]
    // hermes skills disable <name> [--platform <p>]
    // hermes skills enable <name>  [--platform <p>]
    std::string sub = (argc > 2) ? argv[2] : "list";
    auto config = hermes::config::load_config();

    auto find_platform_arg = [&](int start) -> std::string {
        for (int i = start; i < argc; ++i) {
            std::string a = argv[i];
            if (a == "--platform" && i + 1 < argc) return argv[i + 1];
            if (a.rfind("--platform=", 0) == 0) return a.substr(11);
        }
        return {};
    };

    if (sub == "list" || sub == "show") {
        std::string platform = (argc > 3 && std::string(argv[3]).rfind("--", 0) != 0)
                                   ? argv[3]
                                   : find_platform_arg(3);
        return cmd_show(config, platform, std::cout);
    }
    if (sub == "disable" || sub == "enable") {
        if (argc < 4) {
            std::cerr << "Usage: hermes skills " << sub
                      << " <skill> [--platform <p>]\n";
            return 1;
        }
        std::string name = argv[3];
        std::string platform = find_platform_arg(4);
        auto disabled = get_disabled_skills(config, platform);
        if (sub == "disable") disabled.insert(name);
        else disabled.erase(name);
        save_disabled_skills(config, disabled, platform);
        hermes::config::save_config(config);
        std::cout << "Updated disabled list for "
                  << platform_label(platform) << ".\n";
        return 0;
    }
    std::cerr << "Unknown skills subcommand: " << sub << "\n";
    return 1;
}

}  // namespace hermes::cli::skills_config
