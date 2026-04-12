#include "hermes/tools/toolset_distributions.hpp"

#include <gtest/gtest.h>

using hermes::tools::sample_toolsets_from_distribution;

TEST(ToolsetDistributions, DeterministicWithSameSeed) {
    auto a = sample_toolsets_from_distribution("default", 42);
    auto b = sample_toolsets_from_distribution("default", 42);
    EXPECT_EQ(a, b);
    EXPECT_FALSE(a.empty());
}

TEST(ToolsetDistributions, UnknownDistributionThrows) {
    EXPECT_THROW(
        sample_toolsets_from_distribution("nonexistent_xyz", 1),
        std::invalid_argument);
}
