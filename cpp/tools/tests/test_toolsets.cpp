#include "hermes/tools/toolsets.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <stdexcept>

using hermes::tools::hermes_core_tools;
using hermes::tools::resolve_toolset;
using hermes::tools::validate_toolset;

TEST(Toolsets, CoreToolsNonEmpty) {
    EXPECT_FALSE(hermes_core_tools().empty());
}

TEST(Toolsets, ResolveFullStack) {
    auto tools = resolve_toolset("full_stack");
    EXPECT_FALSE(tools.empty());
}

TEST(Toolsets, ResolveUnknownThrows) {
    EXPECT_THROW(resolve_toolset("unknown_nonexistent_xyz"), std::invalid_argument);
}

TEST(Toolsets, ValidateToolsetReturnsDescription) {
    // "full_stack" should be a known toolset; validate returns its description.
    auto desc = validate_toolset("full_stack");
    EXPECT_FALSE(desc.empty());
}

TEST(Toolsets, ValidateUnknownThrows) {
    EXPECT_THROW(validate_toolset("unknown_nonexistent_xyz"), std::invalid_argument);
}
