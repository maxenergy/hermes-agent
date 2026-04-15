// C++17 port of the pure-logic helpers from
// `hermes_cli/memory_setup.py`.

#include "hermes/cli/memory_setup.hpp"

#include <algorithm>
#include <sstream>
#include <unordered_set>

namespace hermes::cli::memory_setup {
namespace {

std::string strip_package_extra(const std::string& pip_name) {
    auto bracket = pip_name.find('[');
    if (bracket == std::string::npos) {
        return pip_name;
    }
    return pip_name.substr(0, bracket);
}

std::string replace_dashes(const std::string& value) {
    std::string out{value};
    std::replace(out.begin(), out.end(), '-', '_');
    return out;
}

std::vector<std::string> split_lines(const std::string& text) {
    std::vector<std::string> out{};
    std::string::size_type start{0};
    while (start <= text.size()) {
        auto nl = text.find('\n', start);
        if (nl == std::string::npos) {
            if (start < text.size()) {
                out.push_back(text.substr(start));
            }
            break;
        }
        out.push_back(text.substr(start, nl - start));
        start = nl + 1;
    }
    return out;
}

std::string trim_copy(const std::string& value) {
    auto is_space = [](unsigned char c) { return std::isspace(c) != 0; };
    auto first = std::find_if_not(value.begin(), value.end(), is_space);
    auto last = std::find_if_not(value.rbegin(), value.rend(), is_space).base();
    if (first >= last) {
        return std::string{};
    }
    return std::string{first, last};
}

}  // namespace

std::string setup_hint_label(setup_hint hint) {
    switch (hint) {
        case setup_hint::requires_api_key:
            return std::string{"requires API key"};
        case setup_hint::api_key_or_local:
            return std::string{"API key / local"};
        case setup_hint::no_setup_needed:
            return std::string{"no setup needed"};
        case setup_hint::local:
            return std::string{"local"};
    }
    return std::string{};
}

setup_hint classify_setup_hint(const std::vector<schema_field>& schema) {
    bool has_secrets{false};
    bool has_non_secrets{false};
    for (const auto& field : schema) {
        if (field.secret) {
            has_secrets = true;
        } else {
            has_non_secrets = true;
        }
    }
    if (has_secrets && has_non_secrets) {
        return setup_hint::api_key_or_local;
    }
    if (has_secrets) {
        return setup_hint::requires_api_key;
    }
    if (schema.empty()) {
        return setup_hint::no_setup_needed;
    }
    return setup_hint::local;
}

const std::unordered_map<std::string, std::string>& pip_import_overrides() {
    static const std::unordered_map<std::string, std::string> table{
        {"honcho-ai", "honcho"},
        {"mem0ai", "mem0"},
        {"hindsight-client", "hindsight_client"},
        {"hindsight-all", "hindsight"},
    };
    return table;
}

std::string pip_to_import_name(const std::string& pip_name) {
    const auto& overrides = pip_import_overrides();
    auto it = overrides.find(pip_name);
    if (it != overrides.end()) {
        return it->second;
    }
    return replace_dashes(strip_package_extra(pip_name));
}

std::vector<std::string> compute_missing_pip_dependencies(
    const std::vector<std::string>& pip_deps,
    const std::function<bool(const std::string&)>& is_installed) {
    std::vector<std::string> out{};
    for (const auto& dep : pip_deps) {
        std::string import_name{pip_to_import_name(dep)};
        bool installed{false};
        if (is_installed) {
            installed = is_installed(import_name);
        }
        if (!installed) {
            out.push_back(dep);
        }
    }
    return out;
}

std::string render_env_file_update(
    const std::string& existing_content,
    const std::vector<std::pair<std::string, std::string>>& updates) {
    std::vector<std::string> existing_lines{split_lines(existing_content)};

    // Build lookup of updates for O(1) matching.
    std::unordered_map<std::string, std::string> update_map{};
    for (const auto& [key, value] : updates) {
        update_map[key] = value;
    }

    std::unordered_set<std::string> updated_keys{};
    std::vector<std::string> new_lines{};
    new_lines.reserve(existing_lines.size() + updates.size());

    for (const auto& line : existing_lines) {
        std::string key{};
        auto eq = line.find('=');
        if (eq != std::string::npos) {
            key = trim_copy(line.substr(0, eq));
        }
        auto it = update_map.find(key);
        if (!key.empty() && it != update_map.end()) {
            new_lines.push_back(key + std::string{"="} + it->second);
            updated_keys.insert(key);
        } else {
            new_lines.push_back(line);
        }
    }

    // Append new keys preserving caller's declared order.
    for (const auto& [key, value] : updates) {
        if (updated_keys.count(key) == 0) {
            new_lines.push_back(key + std::string{"="} + value);
            updated_keys.insert(key);
        }
    }

    std::ostringstream oss{};
    bool first{true};
    for (const auto& line : new_lines) {
        if (!first) {
            oss << '\n';
        }
        first = false;
        oss << line;
    }
    oss << '\n';
    return oss.str();
}

std::string mask_existing_secret(const std::string& value) {
    if (value.size() > 4) {
        return std::string{"..."} + value.substr(value.size() - 4);
    }
    return std::string{"set"};
}

std::string format_provider_config_line(const std::string& key,
                                        const std::string& value) {
    return std::string{"    "} + key + std::string{": "} + value;
}

std::vector<std::string> render_provider_status_lines(const status_context& ctx) {
    std::vector<std::string> lines{};
    lines.emplace_back("");
    lines.emplace_back("Memory status");
    lines.emplace_back(std::string(40, '-'));  // plain dashes -- unicode box in Py
    lines.emplace_back("  Built-in:  always active");
    lines.emplace_back(
        std::string{"  Provider:  "} +
        (ctx.active_provider.empty()
             ? std::string{"(none -- built-in only)"}
             : ctx.active_provider));

    if (!ctx.active_provider.empty()) {
        if (!ctx.active_provider_config.empty()) {
            lines.emplace_back("");
            lines.emplace_back(std::string{"  "} + ctx.active_provider +
                               std::string{" config:"});
            for (const auto& [key, value] : ctx.active_provider_config) {
                lines.push_back(format_provider_config_line(key, value));
            }
        }

        auto found_it = std::find_if(
            ctx.providers.begin(), ctx.providers.end(),
            [&](const discovered_provider& p) {
                return p.name == ctx.active_provider;
            });

        if (found_it != ctx.providers.end()) {
            lines.emplace_back("");
            lines.emplace_back("  Plugin:    installed");
            if (found_it->available) {
                lines.emplace_back("  Status:    available");
            } else {
                lines.emplace_back("  Status:    not available");
                // Secrets list
                if (!found_it->secret_fields.empty()) {
                    lines.emplace_back("  Missing:");
                    for (const auto& s : found_it->secret_fields) {
                        bool is_set{false};
                        if (ctx.env_is_set && !s.env_var.empty()) {
                            is_set = ctx.env_is_set(s.env_var);
                        }
                        std::string mark{is_set ? "[x]" : "[ ]"};
                        std::string line{std::string{"    "} + mark +
                                         std::string{" "} + s.env_var};
                        lines.push_back(line);
                    }
                }
            }
        } else {
            lines.emplace_back("");
            lines.emplace_back("  Plugin:    NOT installed");
            lines.emplace_back(
                std::string{"  Install the '"} + ctx.active_provider +
                std::string{"' memory plugin to ~/.hermes/plugins/"});
        }
    }

    if (!ctx.providers.empty()) {
        lines.emplace_back("");
        lines.emplace_back("  Installed plugins:");
        for (const auto& p : ctx.providers) {
            std::string suffix{};
            if (p.name == ctx.active_provider) {
                suffix = std::string{" <- active"};
            }
            lines.push_back(std::string{"    * "} + p.name +
                            std::string{"  ("} + p.description +
                            std::string{")"} + suffix);
        }
    }

    lines.emplace_back("");
    return lines;
}

}  // namespace hermes::cli::memory_setup
