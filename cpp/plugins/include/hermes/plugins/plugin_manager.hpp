// PluginManager — discovers, loads, and manages plugin shared libraries.
#pragma once

#include "hermes/plugins/plugin.hpp"
#include "hermes/plugins/manifest.hpp"

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
    PluginManifest manifest;     // optional — empty when no plugin.yaml
    std::string error;           // non-empty if load failed
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

    /// Hot-reload — unload, then re-load the plugin from disk.  Fires
    /// `rebuild_lookups_cb_` once at the end.  Returns true if the plugin
    /// was reloaded successfully.
    bool reload(const std::string& name);

    /// Check whether @p manifest permits the plugin to register an
    /// identifier of the given kind.  Kind is one of "tool", "command",
    /// "event", "cli".  Empty capability grants reject everything; the
    /// literal "*" grants unrestricted access.  This is the single enforcement
    /// point for the plugin sandbox.
    static bool capability_allows(const PluginManifest& manifest,
                                  const std::string& kind,
                                  const std::string& identifier);

    /// Scan the plugin directory for `plugin.yaml` manifests (without
    /// loading any shared library).  Used by CLI subcommands like
    /// `list` and `info` that only need metadata.
    std::vector<PluginManifest> scan_manifests() const;

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
