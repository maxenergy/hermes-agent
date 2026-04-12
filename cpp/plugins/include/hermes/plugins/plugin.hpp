// Plugin interface — plugins implement this and export a factory function.
//
// Shared libraries export:
//   extern "C" Plugin* hermes_plugin_create();
//   extern "C" void    hermes_plugin_destroy(Plugin*);
#pragma once

#include <string>

namespace hermes::tools {
class ToolRegistry;  // forward declaration
}

namespace hermes::plugins {

class Plugin {
public:
    virtual ~Plugin() = default;

    virtual std::string name() const = 0;
    virtual std::string version() const = 0;

    /// Called immediately after the shared library is loaded.
    virtual void on_load() {}

    /// Called just before the shared library is unloaded.
    virtual void on_unload() {}

    /// Register tools into the global registry.
    virtual void register_tools(hermes::tools::ToolRegistry& /*registry*/) {}

    /// Register hooks (placeholder — pass an opaque pointer or registry later).
    virtual void register_hooks(/* hermes::gateway::HookRegistry& hooks */) {}
};

// Factory function type aliases for clarity.
using PluginCreateFn  = Plugin* (*)();
using PluginDestroyFn = void (*)(Plugin*);

} // namespace hermes::plugins
