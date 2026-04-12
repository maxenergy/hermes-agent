// OSV (Open Source Vulnerability) advisory check.
//
// Phase 6 ships a STUB that returns an empty advisory list. Phase 8 will
// wire actual osv.dev HTTP requests once the cpr-based transport library
// is available.
#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace hermes::approval {

struct OsvAdvisory {
    std::string id;        // e.g. "GHSA-..."
    std::string summary;
    std::string severity;  // LOW | MEDIUM | HIGH | CRITICAL
    std::vector<std::string> cves;
};

std::vector<OsvAdvisory> check_pypi_package(std::string_view name,
                                            std::string_view version);

}  // namespace hermes::approval
