#include "hermes/config/loader.hpp"

#include "hermes/config/default_config.hpp"
#include "hermes/core/atomic_io.hpp"
#include "hermes/core/path.hpp"

#include <yaml-cpp/yaml.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>

namespace hermes::config {

namespace fs = std::filesystem;

// -----------------------------------------------------------------------
// YAML <-> JSON conversion helpers.
// yaml-cpp doesn't speak nlohmann::json natively, so we walk the YAML
// node tree and emit the equivalent JSON.  Scalars are interpreted
// loosely: `null`, `true`/`false`, integers, doubles, then string
// fallback.  This mirrors the Python `yaml.safe_load` semantics closely
// enough for config files.
// -----------------------------------------------------------------------
namespace {

nlohmann::json yaml_to_json(const YAML::Node& node) {
    switch (node.Type()) {
        case YAML::NodeType::Null:
            return nullptr;
        case YAML::NodeType::Scalar: {
            const auto& s = node.Scalar();
            // YAML tag hint may say it's a plain scalar; attempt typed parses.
            if (s == "null" || s == "~" || s.empty()) {
                return nullptr;
            }
            if (s == "true" || s == "True" || s == "TRUE") {
                return true;
            }
            if (s == "false" || s == "False" || s == "FALSE") {
                return false;
            }
            // Try int
            try {
                std::size_t pos = 0;
                const long long v = std::stoll(s, &pos);
                if (pos == s.size()) {
                    return v;
                }
            } catch (...) {}
            // Try double
            try {
                std::size_t pos = 0;
                const double v = std::stod(s, &pos);
                if (pos == s.size()) {
                    return v;
                }
            } catch (...) {}
            return s;
        }
        case YAML::NodeType::Sequence: {
            nlohmann::json arr = nlohmann::json::array();
            for (const auto& child : node) {
                arr.push_back(yaml_to_json(child));
            }
            return arr;
        }
        case YAML::NodeType::Map: {
            nlohmann::json obj = nlohmann::json::object();
            for (const auto& kv : node) {
                obj[kv.first.as<std::string>()] = yaml_to_json(kv.second);
            }
            return obj;
        }
        case YAML::NodeType::Undefined:
        default:
            return nullptr;
    }
}

YAML::Node json_to_yaml(const nlohmann::json& j) {
    YAML::Node node;
    if (j.is_null()) {
        node = YAML::Node(YAML::NodeType::Null);
    } else if (j.is_boolean()) {
        node = j.get<bool>();
    } else if (j.is_number_integer()) {
        node = j.get<long long>();
    } else if (j.is_number_unsigned()) {
        node = j.get<unsigned long long>();
    } else if (j.is_number_float()) {
        node = j.get<double>();
    } else if (j.is_string()) {
        node = j.get<std::string>();
    } else if (j.is_array()) {
        node = YAML::Node(YAML::NodeType::Sequence);
        for (const auto& item : j) {
            node.push_back(json_to_yaml(item));
        }
    } else if (j.is_object()) {
        node = YAML::Node(YAML::NodeType::Map);
        for (auto it = j.begin(); it != j.end(); ++it) {
            node[it.key()] = json_to_yaml(it.value());
        }
    }
    return node;
}

nlohmann::json deep_merge(nlohmann::json base, const nlohmann::json& overlay) {
    if (!base.is_object() || !overlay.is_object()) {
        return overlay;
    }
    for (auto it = overlay.begin(); it != overlay.end(); ++it) {
        if (base.contains(it.key()) && base[it.key()].is_object() &&
            it.value().is_object()) {
            base[it.key()] = deep_merge(base[it.key()], it.value());
        } else {
            base[it.key()] = it.value();
        }
    }
    return base;
}

nlohmann::json expand_strings(nlohmann::json node) {
    if (node.is_string()) {
        return expand_env_vars(node.get<std::string>());
    }
    if (node.is_object()) {
        for (auto it = node.begin(); it != node.end(); ++it) {
            it.value() = expand_strings(it.value());
        }
        return node;
    }
    if (node.is_array()) {
        for (auto& child : node) {
            child = expand_strings(child);
        }
        return node;
    }
    return node;
}

fs::path config_path() {
    return hermes::core::path::get_hermes_home() / "config.yaml";
}

}  // namespace

// -----------------------------------------------------------------------
// expand_env_vars — `${VAR}` and `${VAR:-default}` only.  Braced form
// required; bare `$VAR` is a literal.  Unresolved `${VAR}` is kept.
// -----------------------------------------------------------------------
std::string expand_env_vars(std::string_view input) {
    std::string out;
    out.reserve(input.size());
    for (std::size_t i = 0; i < input.size();) {
        // Only handle `${...}` — anything else is copied verbatim.
        if (input[i] != '$' || i + 1 >= input.size() || input[i + 1] != '{') {
            out.push_back(input[i]);
            ++i;
            continue;
        }
        const std::size_t start = i + 2;
        const std::size_t end = input.find('}', start);
        if (end == std::string_view::npos) {
            // Unterminated — treat the rest as literal.
            out.append(input.substr(i));
            break;
        }
        const std::string_view body = input.substr(start, end - start);
        std::string var_name;
        std::string fallback;
        bool have_fallback = false;
        const auto sep = body.find(":-");
        if (sep == std::string_view::npos) {
            var_name.assign(body);
        } else {
            var_name.assign(body.substr(0, sep));
            fallback.assign(body.substr(sep + 2));
            have_fallback = true;
        }

        const char* resolved = std::getenv(var_name.c_str());
        if (resolved != nullptr) {
            out.append(resolved);
        } else if (have_fallback) {
            out.append(fallback);
        } else {
            // Leave the original `${VAR}` verbatim.
            out.append(input.substr(i, end - i + 1));
        }
        i = end + 1;
    }
    return out;
}

// -----------------------------------------------------------------------
// load_cli_config — read + deep-merge over defaults + env-expand.
// -----------------------------------------------------------------------
nlohmann::json load_cli_config() {
    nlohmann::json merged = default_config();
    const auto path = config_path();

    std::error_code ec;
    if (!fs::exists(path, ec) || ec) {
        return expand_strings(std::move(merged));
    }

    try {
        const YAML::Node root = YAML::LoadFile(path.string());
        if (root && !root.IsNull()) {
            auto overlay = yaml_to_json(root);
            if (overlay.is_object()) {
                merged = deep_merge(std::move(merged), overlay);
            }
        }
    } catch (const YAML::Exception&) {
        // Corrupt YAML — fall through to defaults. The Python reference
        // prints a warning; we keep quiet to avoid polluting stderr from
        // library code.  TODO(phase-2): surface via logging channel.
    }

    return expand_strings(std::move(merged));
}

nlohmann::json load_config() {
    // Phase 1: identical to load_cli_config.  A divergence will appear
    // when the setup wizard grows migration-on-read semantics.
    return load_cli_config();
}

// -----------------------------------------------------------------------
// save_config — serialise to YAML and atomic_write to ~/.hermes/config.yaml.
// -----------------------------------------------------------------------
void save_config(const nlohmann::json& config) {
    const auto path = config_path();

    std::stringstream buf;
    YAML::Emitter emitter;
    emitter << json_to_yaml(config);
    if (!emitter.good()) {
        throw std::runtime_error(
            std::string("hermes::config::save_config: YAML emit failed: ") +
            emitter.GetLastError());
    }
    buf << emitter.c_str() << '\n';

    if (!hermes::core::atomic_io::atomic_write(path, buf.str())) {
        throw std::runtime_error(
            "hermes::config::save_config: atomic_write failed for " +
            path.string());
    }
}

// -----------------------------------------------------------------------
// detect_managed_system — probes os-release + filesystem.
// -----------------------------------------------------------------------
ManagedSystem detect_managed_system() {
    std::error_code ec;

    // NixOS either ships os-release=nixos or has /nix on the root.
    if (fs::exists("/etc/NIXOS", ec) || fs::exists("/nix/store", ec)) {
        return ManagedSystem::NixOS;
    }

    // Homebrew prefix (arm64 macOS) — simple path probe.
    if (fs::exists("/opt/homebrew", ec) || fs::exists("/home/linuxbrew", ec)) {
        return ManagedSystem::Homebrew;
    }

    // Parse /etc/os-release.
    std::ifstream in("/etc/os-release");
    if (in) {
        std::string line;
        while (std::getline(in, line)) {
            if (line.rfind("ID=", 0) == 0) {
                auto val = line.substr(3);
                // Strip quotes.
                if (!val.empty() && val.front() == '"' && val.back() == '"') {
                    val = val.substr(1, val.size() - 2);
                }
                if (val == "nixos") {
                    return ManagedSystem::NixOS;
                }
                if (val == "debian" || val == "ubuntu") {
                    return ManagedSystem::Debian;
                }
            }
        }
    }

    return ManagedSystem::None;
}

// -----------------------------------------------------------------------
// migrate_config — incremental per-version field migrations.
// -----------------------------------------------------------------------
nlohmann::json migrate_config(nlohmann::json config) {
    if (!config.is_object()) {
        config = nlohmann::json::object();
    }
    int current = 0;
    if (config.contains("_config_version") &&
        config["_config_version"].is_number_integer()) {
        current = config["_config_version"].get<int>();
    }

    // Do not downgrade a config that is already ahead of us.
    if (current >= kCurrentConfigVersion) {
        return config;
    }

    // v1 -> v2: add terminal.backend = "local" if missing
    if (current < 2) {
        if (!config.contains("terminal") || !config["terminal"].is_object()) {
            config["terminal"] = nlohmann::json::object();
        }
        if (!config["terminal"].contains("backend")) {
            config["terminal"]["backend"] = "local";
        }
        config["_config_version"] = 2;
        current = 2;
    }

    // v2 -> v3: rename top-level api_key -> provider_api_key
    if (current < 3) {
        if (config.contains("api_key") && !config.contains("provider_api_key")) {
            config["provider_api_key"] = config["api_key"];
            config.erase("api_key");
        }
        config["_config_version"] = 3;
        current = 3;
    }

    // v3 -> v4: add display.skin = "default" if missing
    if (current < 4) {
        if (!config.contains("display") || !config["display"].is_object()) {
            config["display"] = nlohmann::json::object();
        }
        if (!config["display"].contains("skin")) {
            config["display"]["skin"] = "default";
        }
        config["_config_version"] = 4;
        current = 4;
    }

    // v4 -> v5: add web.backend = "exa" if missing, add tts.provider = "edge" if missing
    if (current < 5) {
        if (!config.contains("web") || !config["web"].is_object()) {
            config["web"] = nlohmann::json::object();
        }
        if (!config["web"].contains("backend")) {
            config["web"]["backend"] = "exa";
        }
        if (!config.contains("tts") || !config["tts"].is_object()) {
            config["tts"] = nlohmann::json::object();
        }
        if (!config["tts"].contains("provider")) {
            config["tts"]["provider"] = "edge";
        }
        config["_config_version"] = 5;
        current = 5;
    }

    return config;
}

}  // namespace hermes::config
