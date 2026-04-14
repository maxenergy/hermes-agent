// Plugin registry state — persists installed + enabled lists between runs.
//
// Stored as JSON at ``~/.hermes/plugins/state.json``.  Schema:
//     {
//       "installed": ["foo", "bar"],
//       "disabled": ["bar"]
//     }
#pragma once

#include <filesystem>
#include <set>
#include <string>

namespace hermes::plugins {

struct PluginState {
    std::set<std::string> installed;
    std::set<std::string> disabled;

    bool is_installed(const std::string& name) const {
        return installed.count(name) > 0;
    }
    bool is_disabled(const std::string& name) const {
        return disabled.count(name) > 0;
    }
    bool is_enabled(const std::string& name) const {
        return is_installed(name) && !is_disabled(name);
    }

    void add_installed(const std::string& name) {
        installed.insert(name);
    }
    void remove_installed(const std::string& name) {
        installed.erase(name);
        disabled.erase(name);
    }
    void disable(const std::string& name) { disabled.insert(name); }
    void enable(const std::string& name) { disabled.erase(name); }
};

/// Load plugin state from the given JSON path.  If the file does not exist
/// (or cannot be parsed), returns an empty state.  Never throws.
PluginState load_state(const std::filesystem::path& json_path);

/// Persist @p state to @p json_path, creating parent directories as
/// needed.  Returns true on success.
bool save_state(const std::filesystem::path& json_path, const PluginState& state);

/// Return the canonical state.json path: ``~/.hermes/plugins/state.json``.
std::filesystem::path default_state_path();

} // namespace hermes::plugins
