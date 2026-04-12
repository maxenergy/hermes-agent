#include "hermes/tools/tool_result.hpp"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <string>

using hermes::tools::standardize;
using hermes::tools::truncate_result;
using json = nlohmann::json;

TEST(ToolResult, TruncateKeepsSuffix) {
    std::string big(200, 'x');
    auto result = truncate_result(big, 50);
    EXPECT_LE(result.size(), 50u);
    EXPECT_NE(result.find("truncated"), std::string::npos);
}

TEST(ToolResult, TruncateNoOpWhenFits) {
    std::string small = "hello";
    EXPECT_EQ(truncate_result(small, 100), small);
}

TEST(ToolResult, StandardizeWrapsNonObject) {
    json arr = json::array({1, 2, 3});
    auto out = standardize(arr);
    EXPECT_TRUE(out.is_object());
    EXPECT_TRUE(out.contains("output"));
    EXPECT_EQ(out["output"], arr);
}

TEST(ToolResult, StandardizeKeepsObject) {
    json obj = {{"key", "value"}};
    auto out = standardize(obj);
    EXPECT_EQ(out, obj);
}

TEST(ToolResult, StandardizeWrapsString) {
    json s = "just a string";
    auto out = standardize(s);
    EXPECT_TRUE(out.is_object());
    EXPECT_EQ(out["output"], "just a string");
}
