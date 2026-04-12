#include "hermes/tools/delegate_tool.hpp"
#include "hermes/tools/registry.hpp"

#include <gtest/gtest.h>

using namespace hermes::tools;

namespace {

class DelegateToolTest : public ::testing::Test {
protected:
    void SetUp() override {
        ToolRegistry::instance().clear();
        register_delegate_tools();
    }
    void TearDown() override { ToolRegistry::instance().clear(); }
};

TEST_F(DelegateToolTest, StubReturnsError) {
    auto result = ToolRegistry::instance().dispatch(
        "delegate_task", {{"goal", "summarize"}}, {});
    auto parsed = nlohmann::json::parse(result);
    EXPECT_TRUE(parsed.contains("error"));
    EXPECT_NE(parsed["error"].get<std::string>().find("not yet wired"),
              std::string::npos);
}

TEST_F(DelegateToolTest, MoAStubReturnsError) {
    auto result = ToolRegistry::instance().dispatch(
        "mixture_of_agents",
        {{"prompt", "hello"}, {"models", nlohmann::json::array({"a", "b"})}},
        {});
    auto parsed = nlohmann::json::parse(result);
    EXPECT_TRUE(parsed.contains("error"));
}

TEST_F(DelegateToolTest, InterfaceCompiles) {
    // Verify the AIAgent interface can be subclassed.
    struct TestAgent : public AIAgent {
        std::string run(const std::string& goal,
                        const std::string& /*constraints*/) override {
            return "done: " + goal;
        }
    };
    TestAgent agent;
    EXPECT_EQ(agent.run("test", ""), "done: test");
}

}  // namespace
