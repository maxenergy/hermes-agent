#include "hermes/config/loader.hpp"

#include "hermes/config/default_config.hpp"
#include "hermes/core/atomic_io.hpp"
#include "hermes/core/logging.hpp"
#include "hermes/core/path.hpp"

#include <yaml-cpp/yaml.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_set>

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
    } catch (const YAML::Exception& ex) {
        // Corrupt YAML — fall through to defaults.
        hermes::core::logging::log_warn(
            std::string("config: failed to parse YAML, using defaults: ") +
            ex.what());
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

    // v5 -> v6: seed security + logging sections (match Python DEFAULT_CONFIG).
    // Preserves any pre-existing user overrides at the field level.
    if (current < 6) {
        if (!config.contains("security") || !config["security"].is_object()) {
            config["security"] = nlohmann::json::object();
        }
        auto& sec = config["security"];
        if (!sec.contains("redact_secrets")) sec["redact_secrets"] = true;
        if (!sec.contains("tirith_enabled")) sec["tirith_enabled"] = true;
        if (!sec.contains("tirith_path")) sec["tirith_path"] = "tirith";
        if (!sec.contains("tirith_timeout")) sec["tirith_timeout"] = 5;
        if (!sec.contains("tirith_fail_open")) sec["tirith_fail_open"] = true;

        if (!config.contains("logging") || !config["logging"].is_object()) {
            config["logging"] = nlohmann::json::object();
        }
        auto& log = config["logging"];
        if (!log.contains("level")) log["level"] = "INFO";
        if (!log.contains("max_size_mb")) log["max_size_mb"] = 5;
        if (!log.contains("backup_count")) log["backup_count"] = 3;

        config["_config_version"] = 6;
        current = 6;
    }

    // v6 -> v7: version-only bump in Python (ENV_VARS_BY_VERSION marker).
    // No schema change — the setup wizard uses it solely to decide which
    // optional env-var prompts to re-surface.  Stamp the version and move on.
    if (current < 7) {
        config["_config_version"] = 7;
        current = 7;
    }

    // v7 -> v8: version-only bump in Python (no schema change).
    if (current < 8) {
        config["_config_version"] = 8;
        current = 8;
    }

    // v8 -> v9: Python clears ANTHROPIC_TOKEN from ~/.hermes/.env.
    // The C++ config loader does not mutate .env files, so the version
    // stamp is advanced and the env-file rewrite is intentionally left to
    // the Python setup wizard (which still runs for mixed installs).
    if (current < 9) {
        config["_config_version"] = 9;
        current = 9;
    }

    // v9 -> v10: version-only bump (TAVILY_API_KEY env var introduced).
    if (current < 10) {
        config["_config_version"] = 10;
        current = 10;
    }

    // v10 -> v11: version-only bump (TERMINAL_MODAL_MODE env var introduced).
    if (current < 11) {
        config["_config_version"] = 11;
        current = 11;
    }

    // v11 -> v12: migrate legacy `custom_providers` list into the
    // `providers` dict.  Each list entry becomes a kebab-cased key under
    // `providers`, with `base_url`/`url` → `api`, `model` → `default_model`,
    // `api_mode` → `transport`.  Placeholder keys ("no-key", "no-key-required",
    // empty) are dropped.  Existing `providers` entries are never overwritten.
    if (current < 12) {
        if (config.contains("custom_providers") &&
            config["custom_providers"].is_array() &&
            !config["custom_providers"].empty()) {
            if (!config.contains("providers") || !config["providers"].is_object()) {
                config["providers"] = nlohmann::json::object();
            }
            auto& providers = config["providers"];
            std::size_t migrated_count = 0;

            auto to_kebab = [](std::string s) {
                std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
                    return static_cast<char>(std::tolower(c));
                });
                std::string out;
                out.reserve(s.size());
                for (char c : s) {
                    if (c == ' ') {
                        out.push_back('-');
                    } else if (c == '(' || c == ')') {
                        // drop
                    } else {
                        out.push_back(c);
                    }
                }
                // collapse consecutive hyphens
                std::string collapsed;
                collapsed.reserve(out.size());
                bool last_hyphen = false;
                for (char c : out) {
                    if (c == '-') {
                        if (!last_hyphen) collapsed.push_back(c);
                        last_hyphen = true;
                    } else {
                        collapsed.push_back(c);
                        last_hyphen = false;
                    }
                }
                // strip leading/trailing hyphens
                std::size_t first = collapsed.find_first_not_of('-');
                std::size_t last = collapsed.find_last_not_of('-');
                if (first == std::string::npos) return std::string{};
                return collapsed.substr(first, last - first + 1);
            };

            auto host_fallback = [](const std::string& url) {
                // naive host extract: scheme://host/... → host; replace dots.
                auto scheme = url.find("://");
                std::string host = (scheme == std::string::npos)
                                       ? url
                                       : url.substr(scheme + 3);
                auto slash = host.find('/');
                if (slash != std::string::npos) host = host.substr(0, slash);
                auto colon = host.find(':');
                if (colon != std::string::npos) host = host.substr(0, colon);
                std::replace(host.begin(), host.end(), '.', '-');
                if (host.empty()) host = "endpoint";
                return host;
            };

            for (const auto& entry : config["custom_providers"]) {
                if (!entry.is_object()) continue;

                auto get_str = [&](const char* key) {
                    if (entry.contains(key) && entry[key].is_string())
                        return entry[key].get<std::string>();
                    return std::string{};
                };

                const std::string old_name = get_str("name");
                std::string old_url = get_str("base_url");
                if (old_url.empty()) old_url = get_str("url");
                const std::string old_key_val = get_str("api_key");
                if (old_url.empty()) continue;  // no URL → skip

                std::string key = to_kebab(old_name);
                if (key.empty()) {
                    key = host_fallback(old_url);
                    if (key.empty()) key = "endpoint-" + std::to_string(migrated_count);
                }
                // Do not overwrite existing entries.
                if (providers.contains(key)) {
                    key = key + "-" + std::to_string(migrated_count);
                }

                nlohmann::json new_entry = nlohmann::json::object();
                new_entry["api"] = old_url;
                if (!old_name.empty()) new_entry["name"] = old_name;
                if (!old_key_val.empty() && old_key_val != "no-key" &&
                    old_key_val != "no-key-required") {
                    new_entry["api_key"] = old_key_val;
                }
                const std::string model = get_str("model");
                if (!model.empty()) new_entry["default_model"] = model;
                const std::string api_mode = get_str("api_mode");
                if (!api_mode.empty()) new_entry["transport"] = api_mode;

                providers[key] = std::move(new_entry);
                ++migrated_count;
            }

            if (migrated_count > 0) {
                config.erase("custom_providers");
            }
        }
        config["_config_version"] = 12;
        current = 12;
    }

    // v12 -> v13: Python clears dead LLM_MODEL / OPENAI_MODEL from .env.
    // Like v8→v9, this is purely an env-file cleanup — no schema change —
    // so the C++ port stamps the version and leaves the env rewrite to the
    // Python setup wizard.
    if (current < 13) {
        config["_config_version"] = 13;
        current = 13;
    }

    // v13 -> v14: migrate legacy flat `stt.model` into the provider-specific
    // section.  Old configs had a provider-agnostic `stt.model`; when the
    // provider was "local", OpenAI model names (e.g. "whisper-1") were fed
    // to faster-whisper and crashed.  Move the value into the right nested
    // section based on `stt.provider`, only if the nested slot isn't set.
    if (current < 14) {
        if (config.contains("stt") && config["stt"].is_object()) {
            auto& stt = config["stt"];
            if (stt.contains("model")) {
                // Pull + remove the flat legacy key.
                nlohmann::json legacy_model = stt["model"];
                stt.erase("model");

                std::string provider = "local";
                if (stt.contains("provider") && stt["provider"].is_string()) {
                    provider = stt["provider"].get<std::string>();
                }

                static const std::unordered_set<std::string> kLocalModels = {
                    "tiny.en", "tiny", "base.en", "base", "small.en", "small",
                    "medium.en", "medium", "large-v1", "large-v2", "large-v3",
                    "large", "distil-large-v2", "distil-medium.en",
                    "distil-small.en", "distil-large-v3", "distil-large-v3.5",
                    "large-v3-turbo", "turbo",
                };

                if (provider == "local" || provider == "local_command") {
                    // Only migrate if legacy value is actually a local model
                    // name.  Cloud names (e.g. "whisper-1") are dropped so
                    // the default "base" takes effect.
                    if (legacy_model.is_string() &&
                        kLocalModels.count(legacy_model.get<std::string>()) > 0) {
                        if (!stt.contains("local") || !stt["local"].is_object()) {
                            stt["local"] = nlohmann::json::object();
                        }
                        if (!stt["local"].contains("model")) {
                            stt["local"]["model"] = legacy_model;
                        }
                    }
                    // else: drop — DEFAULT_CONFIG already defaults local.model
                    // to "base".
                } else {
                    // Cloud provider — place under provider section only if
                    // the user hasn't already specified a nested model.
                    if (!stt.contains(provider) || !stt[provider].is_object()) {
                        stt[provider] = nlohmann::json::object();
                    }
                    if (!stt[provider].contains("model")) {
                        stt[provider]["model"] = legacy_model;
                    }
                }
            }
        }
        config["_config_version"] = 14;
        current = 14;
    }

    return config;
}

}  // namespace hermes::config
