#include "hermes/agent/iteration_budget.hpp"

#include <gtest/gtest.h>

using hermes::agent::IterationBudget;

TEST(IterationBudget, BasicConsume) {
    IterationBudget b(10);
    EXPECT_EQ(b.total(), 10);
    EXPECT_EQ(b.used(), 0);
    EXPECT_EQ(b.remaining(), 10);
    EXPECT_FALSE(b.exhausted());

    b.consume(3);
    EXPECT_EQ(b.used(), 3);
    EXPECT_EQ(b.remaining(), 7);
    EXPECT_FALSE(b.exhausted());

    b.consume(7);
    EXPECT_EQ(b.remaining(), 0);
    EXPECT_TRUE(b.exhausted());
}

TEST(IterationBudget, OvershootSaturates) {
    IterationBudget b(2);
    b.consume(5);
    EXPECT_TRUE(b.exhausted());
    EXPECT_LE(b.remaining(), 0);
}

TEST(IterationBudget, ResetClears) {
    IterationBudget b(4);
    b.consume(2);
    b.reset();
    EXPECT_EQ(b.used(), 0);
    EXPECT_FALSE(b.exhausted());
}

TEST(IterationBudget, NegativeRejected) {
    EXPECT_THROW(IterationBudget(-1), std::invalid_argument);
    IterationBudget b(3);
    EXPECT_THROW(b.consume(-1), std::invalid_argument);
}
