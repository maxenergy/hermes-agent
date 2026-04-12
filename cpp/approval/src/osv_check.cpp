#include "hermes/approval/osv_check.hpp"

#include "hermes/llm/llm_client.hpp"

#include <nlohmann/json.hpp>

#include <string>

namespace hermes::approval {

std::vector<OsvAdvisory> check_pypi_package(std::string_view name,
                                            std::string_view version) {
    auto* transport = hermes::llm::get_default_transport();
    if (!transport) return {};

    nlohmann::json req_body;
    req_body["package"] = {
        {"name", std::string(name)},
        {"ecosystem", "PyPI"}
    };
    req_body["version"] = std::string(version);

    std::unordered_map<std::string, std::string> headers;
    headers["Content-Type"] = "application/json";

    auto resp = transport->post_json(
        "https://api.osv.dev/v1/query", headers, req_body.dump());

    if (resp.status_code != 200) return {};

    auto body = nlohmann::json::parse(resp.body, nullptr, false);
    if (body.is_discarded() || !body.contains("vulns")) return {};

    std::vector<OsvAdvisory> advisories;
    for (const auto& vuln : body["vulns"]) {
        OsvAdvisory adv;
        adv.id = vuln.value("id", "");
        adv.summary = vuln.value("summary", "");

        // Extract severity.
        if (vuln.contains("severity") && !vuln["severity"].empty()) {
            adv.severity = vuln["severity"][0].value("type", "UNKNOWN");
        }

        // Extract CVE aliases.
        if (vuln.contains("aliases") && vuln["aliases"].is_array()) {
            for (const auto& alias : vuln["aliases"]) {
                if (alias.is_string()) {
                    adv.cves.push_back(alias.get<std::string>());
                }
            }
        }

        advisories.push_back(std::move(adv));
    }
    return advisories;
}

}  // namespace hermes::approval
