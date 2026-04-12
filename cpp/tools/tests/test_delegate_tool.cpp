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

TEST_F(DelegateToolTest, NoFactoryReturnsError) {
    auto result = ToolRegistry::instance().dispatch(
        "delegate_task", {{"goal", "summarize"}}, {});
    auto parsed = nlohmann::json::parse(result);
    EXPECT_TRUE(parsed.contains("error"));
    EXPECT_NE(parsed["error"].get<std::string>().find("requires agent factory"),
              std::string::npos);
}

TEST_F(DelegateToolTest, MoANoFactoryReturnsError) {
    auto result = ToolRegistry::instance().dispatch(
        "mixture_of_agents",
        {{"prompt", "hello"}, {"models", nlohmann::json::array({"a", "b"})}},
        {});
    auto parsed = nlohmann::json::parse(result);
    EXPECT_TRUE(parsed.contains("error"));
}

TEST_F(DelegateToolTest, InterfaceCompiles) {
    struct TestAgent : public AIAgent {
        std::string run(const std::string& goal,
                        const std::string& /*constraints*/) override {
            return "done: " + goal;
        }
    };
    TestAgent agent;
    EXPECT_EQ(agent.run("test", ""), "done: test");
}

class DelegateToolWithFactoryTest : public ::testing::Test {
protected:
    void SetUp() override {
        ToolRegistry::instance().clear();
        register_delegate_tools([](const std::string& model) -> std::unique_ptr<AIAgent> {
            struct MockAgent : public AIAgent {
                std::string model_;
                explicit MockAgent(std::string m) : model_(std::move(m)) {}
                std::string run(const std::string& goal,
                                const std::string& /*constraints*/) override {
                    return "result from " + model_ + ": " + goal;
                }
            };
            return std::make_unique<MockAgent>(model.empty() ? "default" : model);
        });
    }
    void TearDown() override { ToolRegistry::instance().clear(); }
};

TEST_F(DelegateToolWithFactoryTest, DelegateTaskReturnsResponse) {
    auto result = ToolRegistry::instance().dispatch(
        "delegate_task",
        {{"goal", "summarize docs"}, {"constraints", "be brief"}},
        {});
    auto parsed = nlohmann::json::parse(result);
    EXPECT_FALSE(parsed.contains("error"));
    EXPECT_TRUE(parsed.contains("response"));
    EXPECT_NE(parsed["response"].get<std::string>().find("summarize"),
              std::string::npos);
    EXPECT_TRUE(parsed["completed"].get<bool>());
}

TEST_F(DelegateToolWithFactoryTest, DelegateTaskWithModel) {
    auto result = ToolRegistry::instance().dispatch(
        "delegate_task",
        {{"goal", "test"}, {"model", "gpt-4o"}},
        {});
    auto parsed = nlohmann::json::parse(result);
    EXPECT_FALSE(parsed.contains("error"));
    EXPECT_NE(parsed["response"].get<std::string>().find("gpt-4o"),
              std::string::npos);
}

TEST_F(DelegateToolWithFactoryTest, MoACallsMultipleModels) {
    auto result = ToolRegistry::instance().dispatch(
        "mixture_of_agents",
        {{"prompt", "hello"}, {"models", nlohmann::json::array({"model-a", "model-b"})}},
        {});
    auto parsed = nlohmann::json::parse(result);
    EXPECT_FALSE(parsed.contains("error"));
    EXPECT_TRUE(parsed.contains("responses"));
    EXPECT_EQ(parsed["responses"].size(), 2u);
    EXPECT_EQ(parsed["model_count"].get<int>(), 2);
    EXPECT_EQ(parsed["responses"][0]["model"].get<std::string>(), "model-a");
    EXPECT_EQ(parsed["responses"][1]["model"].get<std::string>(), "model-b");
}

}  // namespace
