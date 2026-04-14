#include "hermes/plugins/manifest.hpp"

#include <yaml-cpp/yaml.h>

#include <algorithm>
#include <iostream>

namespace hermes::plugins {

namespace {

std::vector<std::string> read_string_list(const YAML::Node& node) {
    std::vector<std::string> out;
    if (!node) return out;
    if (node.IsSequence()) {
        for (const auto& item : node) {
            if (item.IsScalar()) {
                out.push_back(item.as<std::string>());
            } else if (item.IsMap()) {
                // `requires_env` supports list-of-dicts with a "name" key.
                if (item["name"] && item["name"].IsScalar()) {
                    out.push_back(item["name"].as<std::string>());
                }
            }
        }
    } else if (node.IsScalar()) {
        // Allow the legacy single-string form.
        out.push_back(node.as<std::string>());
    }
    return out;
}

Capabilities read_capabilities(const YAML::Node& node) {
    Capabilities caps;
    if (!node || !node.IsMap()) return caps;
    caps.tools    = read_string_list(node["tools"]);
    caps.commands = read_string_list(node["commands"]);
    caps.events   = read_string_list(node["events"]);
    caps.cli      = read_string_list(node["cli"]);
    return caps;
}

} // anonymous namespace

bool Capabilities::allows(const std::vector<std::string>& grants,
                           const std::string& name) {
    for (const auto& g : grants) {
        if (g == "*" || g == name) return true;
    }
    return false;
}

PluginManifest load_manifest(const std::filesystem::path& yaml_path) {
    PluginManifest m;
    try {
        YAML::Node root = YAML::LoadFile(yaml_path.string());
        if (!root || !root.IsMap()) return m;

        if (root["name"]) m.name = root["name"].as<std::string>("");
        if (root["version"]) {
            // `version` may be number or string in yaml; coerce via scalar.
            m.version = root["version"].Scalar();
        }
        if (root["description"]) m.description = root["description"].as<std::string>("");
        if (root["author"]) m.author = root["author"].as<std::string>("");
        if (root["entry"]) m.entry = root["entry"].as<std::string>("");
        if (root["manifest_version"]) {
            try {
                m.manifest_version = root["manifest_version"].as<int>();
            } catch (...) {
                m.manifest_version = 1;
            }
        }
        m.requires_env   = read_string_list(root["requires_env"]);
        m.provides_tools = read_string_list(root["provides_tools"]);
        m.provides_hooks = read_string_list(root["provides_hooks"]);
        m.capabilities   = read_capabilities(root["capabilities"]);
        m.path           = yaml_path.parent_path();
    } catch (const std::exception& e) {
        std::cerr << "plugin manifest parse error (" << yaml_path
                  << "): " << e.what() << "\n";
        return PluginManifest{};
    }
    return m;
}

std::vector<PluginManifest> scan_plugins_dir(const std::filesystem::path& dir,
                                              const std::string& source) {
    std::vector<PluginManifest> out;
    if (!std::filesystem::is_directory(dir)) return out;

    for (const auto& entry : std::filesystem::directory_iterator(dir)) {
        if (!entry.is_directory()) continue;

        auto yaml_path = entry.path() / "plugin.yaml";
        if (!std::filesystem::exists(yaml_path)) {
            yaml_path = entry.path() / "plugin.yml";
        }
        if (!std::filesystem::exists(yaml_path)) continue;

        auto m = load_manifest(yaml_path);
        if (!m.valid()) {
            // Fallback: use directory name as plugin name.
            m.name = entry.path().filename().string();
            m.path = entry.path();
        }
        m.source = source;
        out.push_back(std::move(m));
    }

    std::sort(out.begin(), out.end(),
              [](const PluginManifest& a, const PluginManifest& b) {
                  return a.name < b.name;
              });
    return out;
}

} // namespace hermes::plugins
