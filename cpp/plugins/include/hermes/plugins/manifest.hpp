// Plugin manifest — parsed representation of plugin.yaml.
//
// The manifest declares a plugin's identity, metadata, required environment
// variables, and (critically) its capability grants.  Capabilities form the
// sandbox: a plugin may only register tools, slash commands, hook callbacks,
// and CLI commands whose names are listed in the corresponding capability
// grant.  Anything outside the grant is rejected at register time.
#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace hermes::plugins {

/// Capability grants declared in plugin.yaml.  Each vector lists the
/// concrete identifiers this plugin may touch.  An empty vector means no
/// grant at all (zero permissions); a vector containing the literal "*"
/// means unrestricted access (use sparingly — for trusted first-party
/// plugins only).
struct Capabilities {
    std::vector<std::string> tools;      // tool names the plugin may register
    std::vector<std::string> commands;   // slash-command names
    std::vector<std::string> events;     // lifecycle hook names
    std::vector<std::string> cli;        // `hermes <cmd>` CLI subcommands

    /// Wildcard check — returns true if @p name is in @p grants, or if the
    /// grants vector contains the "*" literal.
    static bool allows(const std::vector<std::string>& grants,
                       const std::string& name);

    bool allows_tool(const std::string& name) const {
        return allows(tools, name);
    }
    bool allows_command(const std::string& name) const {
        return allows(commands, name);
    }
    bool allows_event(const std::string& name) const {
        return allows(events, name);
    }
    bool allows_cli(const std::string& name) const {
        return allows(cli, name);
    }
};

/// Parsed plugin.yaml.  All string fields default to empty.  A manifest is
/// considered valid if @ref name is non-empty.
struct PluginManifest {
    std::string name;
    std::string version;
    std::string description;
    std::string author;
    std::string entry;              // filename of the .so/.dylib/.dll, or empty
    int manifest_version = 1;
    std::vector<std::string> requires_env;
    std::vector<std::string> provides_tools;
    std::vector<std::string> provides_hooks;
    Capabilities capabilities;
    std::filesystem::path path;     // directory that contains plugin.yaml
    std::string source;             // "user" / "project" / "entrypoint"

    bool valid() const { return !name.empty(); }
};

/// Parse a plugin.yaml file.  Returns an empty/invalid manifest on any
/// parse error (errors are logged to std::cerr).
PluginManifest load_manifest(const std::filesystem::path& yaml_path);

/// Scan @p dir for subdirectories containing plugin.yaml (or plugin.yml)
/// and return the list of manifests found, sorted by name.
std::vector<PluginManifest> scan_plugins_dir(const std::filesystem::path& dir,
                                             const std::string& source = "user");

} // namespace hermes::plugins
