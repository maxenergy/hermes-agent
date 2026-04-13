#include "hermes/plugins/plugin_manager.hpp"

#include <algorithm>

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

    const std::string ext = lib_extension();

    bool any_loaded = false;
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
