#include "hermes/tools/managed_tool_gateway.hpp"

#include <string>

namespace hermes::tools {

namespace {

constexpr const char* DEFAULT_DOMAIN = "nousresearch.com";
constexpr const char* DEFAULT_SCHEME = "https";

const nlohmann::json* find_section(const nlohmann::json& cfg, const char* key) {
    if (!cfg.is_object()) return nullptr;
    auto it = cfg.find(key);
    if (it == cfg.end()) return nullptr;
    return &(*it);
}

}  // namespace

bool is_managed_nous_tools_enabled(const nlohmann::json& config) {
    const auto* section = find_section(config, "managed_tool_gateway");
    if (section == nullptr || !section->is_object()) return false;
    auto it = section->find("enabled");
    if (it == section->end()) return false;
    if (it->is_boolean()) return it->get<bool>();
    if (it->is_string()) {
        const std::string s = it->get<std::string>();
        return s == "true" || s == "1" || s == "yes" || s == "on";
    }
    return false;
}

std::optional<VendorGatewayUrl> resolve_vendor_gateway(
    const std::string& vendor, const nlohmann::json& config) {
    if (vendor.empty()) return std::nullopt;
    if (!is_managed_nous_tools_enabled(config)) return std::nullopt;

    const auto* section = find_section(config, "managed_tool_gateway");
    if (section == nullptr) return std::nullopt;

    std::string scheme = DEFAULT_SCHEME;
    std::string domain = DEFAULT_DOMAIN;
    if (auto it = section->find("scheme"); it != section->end() && it->is_string()) {
        scheme = it->get<std::string>();
    }
    if (auto it = section->find("domain"); it != section->end() && it->is_string()) {
        domain = it->get<std::string>();
    }

    std::string proxy;
    if (auto vit = section->find(vendor); vit != section->end()) {
        if (vit->is_string()) {
            proxy = vit->get<std::string>();
        } else if (vit->is_object()) {
            if (auto uit = vit->find("url"); uit != vit->end() && uit->is_string()) {
                proxy = uit->get<std::string>();
            }
        }
    }

    if (proxy.empty()) {
        proxy = scheme + "://" + vendor + "-gateway." + domain;
    }

    VendorGatewayUrl out;
    out.vendor = vendor;
    out.proxy_url = proxy;
    out.enabled = true;
    return out;
}

}  // namespace hermes::tools
