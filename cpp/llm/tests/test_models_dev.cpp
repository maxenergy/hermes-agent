#include "hermes/llm/models_dev.hpp"

#include <gtest/gtest.h>

using namespace hermes::llm::models_dev;

// Without a real HTTP transport, fetch_spec should return nullopt gracefully.

TEST(ModelsDevTest, ReturnsNulloptWithoutTransport) {
    auto result = fetch_spec("gpt-4o");
    EXPECT_FALSE(result.has_value());
}

TEST(ModelsDevTest, ReturnsNulloptForUnknownModel) {
    auto result = fetch_spec("totally-unknown-model-xyz");
    EXPECT_FALSE(result.has_value());
}
