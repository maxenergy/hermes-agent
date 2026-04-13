// Phase 12: Camofox (stealth Firefox) browser backend.
//
// Camofox is a Firefox-based stealth browser that exposes a CDP-like
// debugging protocol.  This header mirrors the public shape the Python
// module exposes; the implementation shells out to the ``camofox-launcher``
// binary when it is present on $PATH and otherwise logs a warning and
// declines to create a backend (returns nullptr from the factory).
#pragma once

#include "hermes/tools/browser_backend.hpp"

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace hermes::tools {

struct BrowserCamofoxConfig {
    std::filesystem::path profile_dir;  // persistent Camofox profile
    std::string proxy;                  // "http://host:port" or "socks5://..."
    std::string user_agent;
    std::string geolocation;            // "lat,lon" or locale ("en-US")
    bool headless = true;
    int debug_port = 9333;
    std::string launcher_path = "camofox-launcher";
    std::vector<std::string> extra_args;
};

/// Persistent state (cookies, fingerprint seed, etc.) serialisable to JSON
/// and stored under ``$HERMES_HOME/browser_auth/camofox``.
struct BrowserCamofoxState {
    std::string user_id;       // stable "hermes_XXXXXXXXXX"
    std::string session_key;   // scoped to a logical task
    std::string fingerprint_seed;  // deterministic seed for consistent fingerprint
    nlohmann::json cookies = nlohmann::json::array();
    nlohmann::json local_storage = nlohmann::json::object();

    nlohmann::json to_json() const;
    static BrowserCamofoxState from_json(const nlohmann::json& j);

    /// Load (or create a fresh) state file for the given profile.
    static BrowserCamofoxState load_for_task(const std::string& task_id);
    /// Persist the current state to the profile directory.
    bool save() const;
};

/// Return the directory where Camofox state is persisted.
std::filesystem::path camofox_state_dir();

/// Return true when the ``camofox-launcher`` binary is on $PATH (or at the
/// absolute path given).
bool camofox_available(const std::string& launcher_path = "camofox-launcher");

/// Factory: launch a Camofox subprocess and return a CDP-backed
/// BrowserBackend.  Returns nullptr when the launcher binary is missing or
/// the subprocess fails to become ready.
std::unique_ptr<BrowserBackend> make_camofox_backend(
    BrowserCamofoxConfig config = {});

/// Install a Camofox backend into the global browser-backend slot when
/// available.  No-op otherwise.  Returns true on success.
bool register_camofox_browser_backend(BrowserCamofoxConfig config = {});

}  // namespace hermes::tools
