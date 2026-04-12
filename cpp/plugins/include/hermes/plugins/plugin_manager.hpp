// PluginManager — discovers, loads, and manages plugin shared libraries.
#pragma once

#include "hermes/plugins/plugin.hpp"

#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace hermes::plugins {

struct PluginInfo {
    std::string name;
    std::string version;
    std::string description;
    std::filesystem::path path;  // .so / .dylib / .dll
    bool enabled = true;
};

class PluginManager {
public:
    /// @param plugins_dir  Directory to scan (e.g. ~/.hermes/plugins/).
    explicit PluginManager(std::filesystem::path plugins_dir);

    /// Scan directory for .so/.dylib files, dlopen each, call factory.
    void discover();

    /// Manual load / unload by plugin name.
    bool load(const std::string& name);
    bool unload(const std::string& name);

    /// Enable / disable (logical toggle — does not load/unload).
    void enable(const std::string& name);
    void disable(const std::string& name);

    /// Query helpers.
    std::vector<PluginInfo> list() const;
    bool is_loaded(const std::string& name) const;

private:
    std::filesystem::path dir_;

    struct LoadedPlugin {
        PluginInfo info;
        void* handle   = nullptr;   // dlopen / LoadLibrary handle
        Plugin* instance = nullptr;
    };

    std::map<std::string, LoadedPlugin> plugins_;

    /// Attempt to dlopen the library at @p path and extract the factory.
    /// Returns nullptr on failure.
    Plugin* try_load_library(const std::filesystem::path& path, void*& handle_out);
};

} // namespace hermes::plugins
