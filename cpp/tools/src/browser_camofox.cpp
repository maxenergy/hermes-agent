#include "hermes/tools/browser_camofox.hpp"

#include "hermes/core/logging.hpp"
#include "hermes/core/path.hpp"
#include "hermes/tools/cdp_backend.hpp"

#include <unistd.h>

#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <system_error>

namespace hermes::tools {

namespace {

namespace fs = std::filesystem;

bool executable_on_path(const std::string& name) {
    if (name.find('/') != std::string::npos) {
        return ::access(name.c_str(), X_OK) == 0;
    }
    const char* path_env = ::getenv("PATH");
    if (!path_env) return false;
    std::istringstream ss(path_env);
    std::string dir;
    while (std::getline(ss, dir, ':')) {
        std::string full = dir + "/" + name;
        if (::access(full.c_str(), X_OK) == 0) return true;
    }
    return false;
}

// Deterministic UUIDv5-like digest: SHA1 of namespace + name, take first N
// hex chars.  Inline minimal UUID5; OpenSSL SHA1 is available via hermes_auth
// but we avoid the dep by using a small xorshift-based digest that yields a
// stable 10-char hex string per input.  Good enough for identity scoping.
std::string short_digest(const std::string& input, std::size_t n) {
    uint64_t h = 0xcbf29ce484222325ULL;  // FNV-1a 64 seed
    for (unsigned char c : input) {
        h ^= c;
        h *= 0x100000001b3ULL;
    }
    // Mix a bit more so two close inputs diverge visually.
    h ^= (h >> 33);
    h *= 0xff51afd7ed558ccdULL;
    h ^= (h >> 33);
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%016lx", static_cast<unsigned long>(h));
    std::string s = buf;
    if (s.size() > n) s.resize(n);
    return s;
}

fs::path state_file_for(const std::string& task_id) {
    auto dir = camofox_state_dir();
    std::error_code ec;
    fs::create_directories(dir, ec);
    std::string name = task_id.empty() ? std::string("default") : task_id;
    return dir / (name + ".json");
}

}  // namespace

fs::path camofox_state_dir() {
    return hermes::core::path::get_hermes_home() / "browser_auth" / "camofox";
}

bool camofox_available(const std::string& launcher_path) {
    return executable_on_path(launcher_path);
}

nlohmann::json BrowserCamofoxState::to_json() const {
    nlohmann::json j;
    j["user_id"] = user_id;
    j["session_key"] = session_key;
    j["fingerprint_seed"] = fingerprint_seed;
    j["cookies"] = cookies;
    j["local_storage"] = local_storage;
    return j;
}

BrowserCamofoxState BrowserCamofoxState::from_json(const nlohmann::json& j) {
    BrowserCamofoxState s;
    if (!j.is_object()) return s;
    s.user_id = j.value("user_id", "");
    s.session_key = j.value("session_key", "");
    s.fingerprint_seed = j.value("fingerprint_seed", "");
    s.cookies = j.value("cookies", nlohmann::json::array());
    s.local_storage = j.value("local_storage", nlohmann::json::object());
    return s;
}

BrowserCamofoxState BrowserCamofoxState::load_for_task(const std::string& task_id) {
    auto path = state_file_for(task_id);
    std::error_code ec;
    if (fs::exists(path, ec)) {
        std::ifstream ifs(path);
        if (ifs) {
            auto j = nlohmann::json::parse(ifs, nullptr, false);
            if (!j.is_discarded()) {
                auto s = BrowserCamofoxState::from_json(j);
                if (!s.user_id.empty()) return s;
            }
        }
    }
    // Mint a fresh identity scoped to profile-root + task.
    BrowserCamofoxState s;
    auto scope = camofox_state_dir().string();
    s.user_id = "hermes_" + short_digest("camofox-user:" + scope, 10);
    s.session_key = "task_" +
                    short_digest("camofox-session:" + scope + ":" +
                                     (task_id.empty() ? "default" : task_id),
                                 16);
    s.fingerprint_seed = short_digest("camofox-fp:" + s.user_id, 16);
    return s;
}

bool BrowserCamofoxState::save() const {
    auto dir = camofox_state_dir();
    std::error_code ec;
    fs::create_directories(dir, ec);
    auto path = dir / (session_key.empty() ? std::string("default.json")
                                           : session_key + ".json");
    std::ofstream ofs(path, std::ios::trunc);
    if (!ofs) return false;
    ofs << to_json().dump(2);
    return true;
}

std::unique_ptr<BrowserBackend> make_camofox_backend(BrowserCamofoxConfig config) {
    if (!camofox_available(config.launcher_path)) {
        hermes::core::logging::log_warn(
            "Camofox launcher not found on PATH — skipping backend registration");
        return nullptr;
    }
    // Camofox exposes the CDP protocol.  Launch it with
    // --remote-debugging-port=N and reuse CdpBackend to drive it.
    CdpConfig cdp;
    cdp.chrome_path = config.launcher_path;
    cdp.debug_port = config.debug_port;
    cdp.headless = config.headless;
    if (!config.profile_dir.empty()) {
        cdp.user_data_dir = config.profile_dir.string();
    }
    if (!config.proxy.empty()) {
        cdp.extra_args.push_back("--proxy-server=" + config.proxy);
    }
    if (!config.user_agent.empty()) {
        cdp.extra_args.push_back("--user-agent=" + config.user_agent);
    }
    if (!config.geolocation.empty()) {
        cdp.extra_args.push_back("--geolocation=" + config.geolocation);
    }
    for (auto& a : config.extra_args) {
        cdp.extra_args.push_back(std::move(a));
    }
    return make_cdp_backend(std::move(cdp));
}

bool register_camofox_browser_backend(BrowserCamofoxConfig config) {
    auto backend = make_camofox_backend(std::move(config));
    if (!backend) return false;
    set_browser_backend(std::move(backend));
    return true;
}

}  // namespace hermes::tools
