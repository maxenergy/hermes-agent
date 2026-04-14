#include "hermes/plugins/state.hpp"

#include "hermes/core/path.hpp"

#include <nlohmann/json.hpp>

#include <fstream>
#include <iostream>

namespace hermes::plugins {

std::filesystem::path default_state_path() {
    return hermes::core::path::get_hermes_home() / "plugins" / "state.json";
}

PluginState load_state(const std::filesystem::path& json_path) {
    PluginState s;
    if (!std::filesystem::exists(json_path)) return s;

    try {
        std::ifstream ifs(json_path);
        nlohmann::json j;
        ifs >> j;

        if (j.contains("installed") && j["installed"].is_array()) {
            for (const auto& name : j["installed"]) {
                if (name.is_string()) {
                    s.installed.insert(name.get<std::string>());
                }
            }
        }
        if (j.contains("disabled") && j["disabled"].is_array()) {
            for (const auto& name : j["disabled"]) {
                if (name.is_string()) {
                    s.disabled.insert(name.get<std::string>());
                }
            }
        }
    } catch (const std::exception& e) {
        std::cerr << "plugin state.json parse error: " << e.what() << "\n";
        return PluginState{};
    }
    return s;
}

bool save_state(const std::filesystem::path& json_path,
                const PluginState& state) {
    try {
        std::filesystem::create_directories(json_path.parent_path());
        nlohmann::json j;
        j["installed"] = nlohmann::json::array();
        for (const auto& name : state.installed) {
            j["installed"].push_back(name);
        }
        j["disabled"] = nlohmann::json::array();
        for (const auto& name : state.disabled) {
            j["disabled"].push_back(name);
        }
        std::ofstream ofs(json_path);
        ofs << j.dump(2);
        return static_cast<bool>(ofs);
    } catch (const std::exception& e) {
        std::cerr << "plugin state.json save error: " << e.what() << "\n";
        return false;
    }
}

} // namespace hermes::plugins
