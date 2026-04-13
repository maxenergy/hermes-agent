#include "hermes/tools/tool_backend_helpers.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <string>

namespace hermes::tools::backend_helpers {

namespace {

std::string lower_trim(std::string s) {
    // trim whitespace
    auto not_ws = [](unsigned char c) { return !std::isspace(c); };
    auto b = std::find_if(s.begin(), s.end(), not_ws);
    auto e = std::find_if(s.rbegin(), s.rend(), not_ws).base();
    if (b >= e) return {};
    std::string out(b, e);
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    return out;
}

bool env_bool(const char* name) {
    const char* v = std::getenv(name);
    if (!v || !*v) return false;
    std::string s = lower_trim(v);
    return s == "1" || s == "true" || s == "yes" || s == "on";
}

std::string getenv_str(const char* name) {
    const char* v = std::getenv(name);
    return v ? std::string(v) : std::string{};
}

}  // namespace

bool managed_nous_tools_enabled() {
    return env_bool("HERMES_ENABLE_NOUS_MANAGED_TOOLS");
}

std::string normalize_browser_cloud_provider(const std::string& value) {
    std::string v = lower_trim(value);
    if (v.empty()) return "local";
    return v;
}

std::string coerce_modal_mode(const std::string& value) {
    std::string v = lower_trim(value);
    if (v == "auto" || v == "direct" || v == "managed") return v;
    return "auto";
}

bool has_direct_modal_credentials() {
    auto tid = getenv_str("MODAL_TOKEN_ID");
    auto tsec = getenv_str("MODAL_TOKEN_SECRET");
    if (!tid.empty() && !tsec.empty()) return true;
    const char* home = std::getenv("HOME");
    if (home && *home) {
        std::error_code ec;
        if (std::filesystem::exists(std::filesystem::path(home) / ".modal.toml", ec)) {
            return true;
        }
    }
    return false;
}

ModalBackendState resolve_modal_backend_state(const std::string& modal_mode,
                                              bool has_direct,
                                              bool managed_ready) {
    ModalBackendState out;
    out.requested_mode = coerce_modal_mode(modal_mode);
    out.mode = out.requested_mode;
    out.has_direct = has_direct;
    out.managed_ready = managed_ready;
    const bool managed_ok = managed_nous_tools_enabled();
    out.managed_mode_blocked = (out.requested_mode == "managed" && !managed_ok);

    if (out.mode == "managed") {
        if (managed_ok && managed_ready) out.selected_backend = "managed";
    } else if (out.mode == "direct") {
        if (has_direct) out.selected_backend = "direct";
    } else {  // auto
        if (managed_ok && managed_ready) {
            out.selected_backend = "managed";
        } else if (has_direct) {
            out.selected_backend = "direct";
        }
    }
    return out;
}

std::string select_backend(const std::string& tool_name,
                           const nlohmann::json& config) {
    if (config.is_object()) {
        auto it = config.find("mock_tools");
        if (it != config.end() && it->is_array()) {
            for (const auto& v : *it) {
                if (v.is_string() && v.get<std::string>() == tool_name) {
                    return "mock";
                }
            }
        }
    }
    // Managed gateway enabled → route through it, otherwise default.
    bool managed = managed_nous_tools_enabled();
    if (!managed && config.is_object()) {
        auto it = config.find("use_managed_gateway");
        if (it != config.end() && it->is_boolean() && it->get<bool>()) {
            managed = true;
        }
    }
    return managed ? "managed_gateway" : "default";
}

std::optional<std::string> resolve_vendor_endpoint(const std::string& vendor,
                                                   const nlohmann::json& config) {
    if (!config.is_object()) return std::nullopt;
    auto it = config.find("vendor_endpoints");
    if (it != config.end() && it->is_object()) {
        auto v = it->find(vendor);
        if (v != it->end() && v->is_string()) {
            return v->get<std::string>();
        }
    }
    // Fallback: Nous-managed base URL + vendor.
    auto base_it = config.find("managed_gateway_base");
    if (base_it != config.end() && base_it->is_string()) {
        std::string base = base_it->get<std::string>();
        if (!base.empty() && base.back() == '/') base.pop_back();
        return base + "/" + vendor;
    }
    // Env fallback.
    auto env_base = getenv_str("HERMES_NOUS_GATEWAY_BASE");
    if (!env_base.empty()) {
        if (env_base.back() == '/') env_base.pop_back();
        return env_base + "/" + vendor;
    }
    return std::nullopt;
}

void apply_backend_headers(
    std::unordered_map<std::string, std::string>& headers,
    const std::string& backend,
    const nlohmann::json& config) {
    if (backend == "managed_gateway") {
        headers["X-Nous-Client"] = "hermes-cpp";
        if (config.is_object()) {
            auto it = config.find("nous_api_key");
            if (it != config.end() && it->is_string()) {
                headers["X-Nous-API-Key"] = it->get<std::string>();
            }
            auto pid = config.find("profile_id");
            if (pid != config.end() && pid->is_string()) {
                headers["X-Nous-Profile"] = pid->get<std::string>();
            }
        }
        auto env_key = getenv_str("HERMES_NOUS_API_KEY");
        if (!env_key.empty() && headers.find("X-Nous-API-Key") == headers.end()) {
            headers["X-Nous-API-Key"] = env_key;
        }
    }
    // "default" / "mock" need no extra headers.
}

std::string resolve_openai_audio_api_key() {
    auto v = getenv_str("VOICE_TOOLS_OPENAI_KEY");
    if (!v.empty()) return v;
    return getenv_str("OPENAI_API_KEY");
}

}  // namespace hermes::tools::backend_helpers
