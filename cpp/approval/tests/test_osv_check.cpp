#include "hermes/approval/osv_check.hpp"

#include <gtest/gtest.h>

using namespace hermes::approval;

// check_pypi_package should not crash for any input, and returns a
// (possibly non-empty) list of advisories.

TEST(OsvCheckTest, DoesNotCrashForKnownPackage) {
    // May return real advisories if transport is available, or empty if not.
    auto advisories = check_pypi_package("requests", "2.28.0");
    // Just verify no crash; we don't assert empty since a real transport
    // will contact osv.dev and return actual CVEs.
    (void)advisories;
}

TEST(OsvCheckTest, EmptyNameReturnsEmpty) {
    auto advisories = check_pypi_package("", "1.0.0");
    EXPECT_TRUE(advisories.empty());
}

TEST(OsvCheckTest, AdvisoryFieldsPopulatedWhenAvailable) {
    // If the transport is available and requests 2.28.0 has known vulns,
    // the advisory fields should be non-empty.
    auto advisories = check_pypi_package("requests", "2.28.0");
    for (const auto& adv : advisories) {
        EXPECT_FALSE(adv.id.empty());
        // summary and severity may be empty for some advisories
    }
}
