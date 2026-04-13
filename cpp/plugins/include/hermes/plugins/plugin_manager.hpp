// PluginManager — discovers, loads, and manages plugin shared libraries.
#pragma once

#include "hermes/plugins/plugin.hpp"

#include <filesystem>
#include <functional>
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

    /// Callback invoked once after any batch of plugins finishes registering
    /// (end of `discover()`, end of a successful `load()`, and after
    /// `unload()` completes).  Intended to wire into
    /// `hermes::cli::rebuild_lookups()` so downstream command / tool
    /// lookup caches are refreshed in a single place.  No-op if unset.
    using RebuildLookupsFn = std::function<void()>;
    void set_rebuild_lookups_cb(RebuildLookupsFn cb) {
        rebuild_lookups_cb_ = std::move(cb);
    }

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

    RebuildLookupsFn rebuild_lookups_cb_;

    /// Fire the registered rebuild callback, if any.  Safe to call when
    /// no callback has been set.
    void fire_rebuild_lookups() {
        if (rebuild_lookups_cb_) rebuild_lookups_cb_();
    }

    /// Attempt to dlopen the library at @p path and extract the factory.
    /// Returns nullptr on failure.
    Plugin* try_load_library(const std::filesystem::path& path, void*& handle_out);
};

} // namespace hermes::plugins
