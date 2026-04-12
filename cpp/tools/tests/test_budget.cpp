#include "hermes/tools/budget_config.hpp"

#include <gtest/gtest.h>

TEST(BudgetConfig, DefaultResultSizePositive) {
    EXPECT_GT(hermes::tools::DEFAULT_RESULT_SIZE_CHARS, 0u);
}

TEST(BudgetConfig, DefaultMaxToolCallsPerTurnPositive) {
    EXPECT_GT(hermes::tools::DEFAULT_MAX_TOOL_CALLS_PER_TURN, 0);
}

TEST(BudgetConfig, DefaultTurnBudgetPositive) {
    EXPECT_GT(hermes::tools::DEFAULT_TURN_BUDGET_CHARS, 0u);
}

TEST(BudgetConfig, DefaultPreviewSizePositive) {
    EXPECT_GT(hermes::tools::DEFAULT_PREVIEW_SIZE_CHARS, 0u);
}
