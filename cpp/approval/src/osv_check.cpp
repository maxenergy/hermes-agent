#include "hermes/approval/osv_check.hpp"

namespace hermes::approval {

// Phase 6 stub. Phase 8 will replace this with a cpr-based osv.dev call.
std::vector<OsvAdvisory> check_pypi_package(std::string_view /*name*/,
                                            std::string_view /*version*/) {
    return {};
}

}  // namespace hermes::approval
