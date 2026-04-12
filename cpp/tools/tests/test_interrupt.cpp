#include "hermes/tools/interrupt.hpp"

#include <gtest/gtest.h>

using hermes::tools::InterruptFlag;

TEST(InterruptFlag, RequestAndClear) {
    auto& flag = InterruptFlag::global();
    flag.clear();
    EXPECT_FALSE(flag.requested());

    flag.request();
    EXPECT_TRUE(flag.requested());

    flag.clear();
    EXPECT_FALSE(flag.requested());
}

TEST(InterruptFlag, GlobalIsSingleton) {
    auto& a = InterruptFlag::global();
    auto& b = InterruptFlag::global();
    EXPECT_EQ(&a, &b);
}
