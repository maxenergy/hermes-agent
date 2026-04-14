#include "hermes/plugins/plugin_manager.hpp"

#include "hermes/plugins/state.hpp"

#include <algorithm>
#include <iostream>

// Platform-specific dynamic library helpers.
#ifdef _WIN32
#  include <windows.h>
#else
#  include <dlfcn.h>
#endif

namespace hermes::plugins {

// ---------------------------------------------------------------------------
// Platform abstraction
// ---------------------------------------------------------------------------

namespace {

void* lib_open(const std::filesystem::path& path) {
#ifdef _WIN32
    return static_cast<void*>(LoadLibraryA(path.string().c_str()));
#else
    return dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
#endif
}

void lib_close(void* handle) {
    if (!handle) return;
#ifdef _WIN32
    FreeLibrary(static_cast<HMODULE>(handle));
#else
    dlclose(handle);
#endif
}

template <typename Fn>
Fn lib_sym(void* handle, const char* symbol) {
#ifdef _WIN32
    return reinterpret_cast<Fn>(
        GetProcAddress(static_cast<HMODULE>(handle), symbol));
#else
    return reinterpret_cast<Fn>(dlsym(handle, symbol));
#endif
}

/// Return the shared library file extension for the current platform.
const char* lib_extension() {
#ifdef _WIN32
    return ".dll";
#elif defined(__APPLE__)
    return ".dylib";
#else
    return ".so";
#endif
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// PluginManager
// ---------------------------------------------------------------------------

PluginManager::PluginManager(std::filesystem::path plugins_dir)
    : dir_(std::move(plugins_dir)) {}

void PluginManager::discover() {
    if (!std::filesystem::is_directory(dir_)) return;

    // Honour state.json's disabled list.
    auto state_path = dir_ / "state.json";
    PluginState state = load_state(state_path);

    const std::string ext = lib_extension();
    bool any_loaded = false;

    // ----- Pass 1: directory-based plugins (with plugin.yaml manifests) ---
    for (const auto& entry : std::filesystem::directory_iterator(dir_)) {
        if (!entry.is_directory()) continue;

        auto yaml_path = entry.path() / "plugin.yaml";
        if (!std::filesystem::exists(yaml_path)) {
            yaml_path = entry.path() / "plugin.yml";
        }
        if (!std::filesystem::exists(yaml_path)) continue;

        auto manifest = load_manifest(yaml_path);
        if (!manifest.valid()) {
            manifest.name = entry.path().filename().string();
            manifest.path = entry.path();
        }
        manifest.source = "user";

        // Respect the user's disabled list.
        if (state.is_disabled(manifest.name)) {
            LoadedPlugin lp;
            lp.info.name     = manifest.name;
            lp.info.version  = manifest.version;
            lp.info.path     = entry.path();
            lp.info.enabled  = false;
            lp.info.manifest = manifest;
            lp.info.error    = "disabled";
            plugins_[manifest.name] = lp;
            continue;
        }

        // Locate the shared library: prefer manifest.entry; fall back to
        // any <ext> file in the directory.
        std::filesystem::path lib_path;
        if (!manifest.entry.empty()) {
            lib_path = entry.path() / manifest.entry;
        }
        if (lib_path.empty() || !std::filesystem::exists(lib_path)) {
            for (const auto& f : std::filesystem::directory_iterator(entry.path())) {
                if (f.is_regular_file() && f.path().extension().string() == ext) {
                    lib_path = f.path();
                    break;
                }
            }
        }

        LoadedPlugin lp;
        lp.info.name     = manifest.name;
        lp.info.version  = manifest.version;
        lp.info.path     = lib_path;
        lp.info.manifest = manifest;

        if (lib_path.empty() || !std::filesystem::exists(lib_path)) {
            // Manifest-only plugin (no .so yet, e.g. needs building).  Record
            // it so `list` shows something, but don't attempt to dlopen.
            lp.info.enabled = false;
            lp.info.error   = "no shared library";
            plugins_[manifest.name] = lp;
            continue;
        }

        void* handle = nullptr;
        Plugin* inst = try_load_library(lib_path, handle);
        if (!inst) {
            lp.info.enabled = false;
            lp.info.error   = "dlopen failed";
            plugins_[manifest.name] = lp;
            continue;
        }

        inst->on_load();
        inst->register_commands();

        lp.info.enabled = true;
        lp.handle       = handle;
        lp.instance     = inst;
        plugins_[manifest.name] = lp;
        any_loaded = true;
    }

    // ----- Pass 2: top-level .so files (legacy / manifest-less) -----------
    for (const auto& entry : std::filesystem::directory_iterator(dir_)) {
        if (!entry.is_regular_file()) continue;
        if (entry.path().extension().string() != ext) continue;

        void* handle = nullptr;
        Plugin* inst = try_load_library(entry.path(), handle);
        if (!inst) continue;

        inst->on_load();
        inst->register_commands();

        LoadedPlugin lp;
        lp.info.name    = inst->name();
        lp.info.version = inst->version();
        lp.info.path    = entry.path();
        lp.info.enabled = true;
        lp.handle       = handle;
        lp.instance     = inst;

        plugins_[lp.info.name] = lp;
        any_loaded = true;
    }

    // Rebuild lookup caches once for the whole batch.
    if (any_loaded) fire_rebuild_lookups();
}

bool PluginManager::load(const std::string& name) {
    // Already loaded?
    if (plugins_.count(name) && plugins_[name].instance) return true;

    // Attempt to find a library file named <name><ext> in the directory.
    const auto path = dir_ / (name + lib_extension());
    if (!std::filesystem::exists(path)) return false;

    void* handle = nullptr;
    Plugin* inst = try_load_library(path, handle);
    if (!inst) return false;

    inst->on_load();
    inst->register_commands();

    LoadedPlugin lp;
    lp.info.name    = inst->name();
    lp.info.version = inst->version();
    lp.info.path    = path;
    lp.info.enabled = true;
    lp.handle       = handle;
    lp.instance     = inst;

    plugins_[name] = lp;
    fire_rebuild_lookups();
    return true;
}

bool PluginManager::unload(const std::string& name) {
    auto it = plugins_.find(name);
    if (it == plugins_.end()) return false;

    auto& lp = it->second;
    if (lp.instance) {
        lp.instance->on_unload();

        // Look up the destroy function.
        if (lp.handle) {
            auto destroy_fn =
                lib_sym<PluginDestroyFn>(lp.handle, "hermes_plugin_destroy");
            if (destroy_fn) {
                destroy_fn(lp.instance);
            }
        }
        lp.instance = nullptr;
    }

    if (lp.handle) {
        lib_close(lp.handle);
        lp.handle = nullptr;
    }

    plugins_.erase(it);
    fire_rebuild_lookups();
    return true;
}

void PluginManager::enable(const std::string& name) {
    auto it = plugins_.find(name);
    if (it != plugins_.end()) {
        it->second.info.enabled = true;
    }
}

void PluginManager::disable(const std::string& name) {
    auto it = plugins_.find(name);
    if (it != plugins_.end()) {
        it->second.info.enabled = false;
    }
}

std::vector<PluginInfo> PluginManager::list() const {
    std::vector<PluginInfo> result;
    result.reserve(plugins_.size());
    for (const auto& [key, lp] : plugins_) {
        result.push_back(lp.info);
    }
    return result;
}

bool PluginManager::is_loaded(const std::string& name) const {
    auto it = plugins_.find(name);
    return it != plugins_.end() && it->second.instance != nullptr;
}

bool PluginManager::reload(const std::string& name) {
    // Remember the library path before unloading (so we can reopen).
    std::filesystem::path saved_path;
    auto it = plugins_.find(name);
    if (it != plugins_.end()) {
        saved_path = it->second.info.path;
        unload(name);  // fires rebuild_lookups once
    }

    // Re-run discover-style logic against the directory the plugin lives in.
    // If we know the path, try that specifically; else fall back to load().
    if (!saved_path.empty() && std::filesystem::exists(saved_path)) {
        void* handle = nullptr;
        Plugin* inst = try_load_library(saved_path, handle);
        if (!inst) return false;
        inst->on_load();
        inst->register_commands();
        LoadedPlugin lp;
        lp.info.name    = inst->name();
        lp.info.version = inst->version();
        lp.info.path    = saved_path;
        lp.info.enabled = true;
        lp.handle       = handle;
        lp.instance     = inst;
        plugins_[lp.info.name] = lp;
        fire_rebuild_lookups();
        return true;
    }

    return load(name);
}

bool PluginManager::capability_allows(const PluginManifest& manifest,
                                       const std::string& kind,
                                       const std::string& identifier) {
    if (kind == "tool")    return manifest.capabilities.allows_tool(identifier);
    if (kind == "command") return manifest.capabilities.allows_command(identifier);
    if (kind == "event")   return manifest.capabilities.allows_event(identifier);
    if (kind == "cli")     return manifest.capabilities.allows_cli(identifier);
    // Unknown kind — reject (fail-closed).
    return false;
}

std::vector<PluginManifest> PluginManager::scan_manifests() const {
    return scan_plugins_dir(dir_, "user");
}

Plugin* PluginManager::try_load_library(const std::filesystem::path& path,
                                         void*& handle_out) {
    handle_out = lib_open(path);
    if (!handle_out) return nullptr;

    auto create_fn =
        lib_sym<PluginCreateFn>(handle_out, "hermes_plugin_create");
    if (!create_fn) {
        lib_close(handle_out);
        handle_out = nullptr;
        return nullptr;
    }

    return create_fn();
}

} // namespace hermes::plugins
