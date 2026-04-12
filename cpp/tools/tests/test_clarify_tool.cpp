#include "hermes/tools/clarify_tool.hpp"
#include "hermes/tools/registry.hpp"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

using namespace hermes::tools;

namespace {

class ClarifyToolTest : public ::testing::Test {
protected:
    void SetUp() override {
        ToolRegistry::instance().clear();
        clear_clarify_callback();
        register_clarify_tools(ToolRegistry::instance());
    }

    void TearDown() override {
        ToolRegistry::instance().clear();
        clear_clarify_callback();
    }

    std::string dispatch(const nlohmann::json& args) {
        return ToolRegistry::instance().dispatch("clarify", args, ctx_);
    }

    ToolContext ctx_{"task1", "test", "s1", "cli", "/tmp", {}};
};

TEST_F(ClarifyToolTest, NoCallbackReturnsError) {
    auto r = nlohmann::json::parse(
        dispatch({{"question", "Which option?"}}));
    EXPECT_TRUE(r.contains("error"));
    EXPECT_NE(r["error"].get<std::string>().find("no clarify callback"),
              std::string::npos);
}

TEST_F(ClarifyToolTest, CallbackReturnsAnswer) {
    set_clarify_callback(
        [](const std::string& /*q*/,
           const std::vector<std::string>& /*c*/) -> std::string {
            return "option B";
        });

    auto r = nlohmann::json::parse(
        dispatch({{"question", "Pick one"}, {"choices", {"A", "B", "C"}}}));
    EXPECT_EQ(r["answer"].get<std::string>(), "option B");
}

TEST_F(ClarifyToolTest, Max4ChoicesEnforced) {
    set_clarify_callback(
        [](const std::string& /*q*/,
           const std::vector<std::string>& choices) -> std::string {
            return choices.empty() ? "none" : choices[0];
        });

    // 4 choices should work fine
    auto r = nlohmann::json::parse(
        dispatch({{"question", "Pick"},
                  {"choices", {"A", "B", "C", "D"}}}));
    EXPECT_FALSE(r.contains("error"));
    EXPECT_EQ(r["answer"].get<std::string>(), "A");
}

}  // namespace
