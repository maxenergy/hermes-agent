#include "hermes/llm/llm_client.hpp"
#include "hermes/tools/registry.hpp"
#include "hermes/tools/rl_training_tool.hpp"

#include <gtest/gtest.h>

#include <cstdlib>

using namespace hermes::tools;
using namespace hermes::llm;

namespace {

class RlToolsTest : public ::testing::Test {
protected:
    void SetUp() override {
        ToolRegistry::instance().clear();
        fake_ = std::make_unique<FakeHttpTransport>();
        // Set env vars so check_fn passes.
        setenv("NOUS_RL_API_URL", "http://localhost:9999", 1);
        setenv("NOUS_RL_API_KEY", "test-key-123", 1);
        register_rl_tools(fake_.get());
    }
    void TearDown() override {
        ToolRegistry::instance().clear();
        unsetenv("NOUS_RL_API_URL");
        unsetenv("NOUS_RL_API_KEY");
    }

    std::unique_ptr<FakeHttpTransport> fake_;
};

TEST_F(RlToolsTest, ListEnvironmentsParsesResponse) {
    nlohmann::json body = {
        {"environments", {{{"name", "cartpole"}, {"version", "1.0"}}}}};
    fake_->enqueue_response({200, body.dump(), {}});

    auto result = ToolRegistry::instance().dispatch(
        "rl_list_environments", nlohmann::json::object(), {});
    auto parsed = nlohmann::json::parse(result);
    ASSERT_TRUE(parsed.contains("environments"));
    EXPECT_EQ(parsed["environments"][0]["name"], "cartpole");

    // Verify request URL.
    ASSERT_EQ(fake_->requests().size(), 1u);
    EXPECT_NE(fake_->requests()[0].url.find("/environments"),
              std::string::npos);
}

TEST_F(RlToolsTest, SelectEnvironmentSendsName) {
    fake_->enqueue_response({200, R"({"selected":true})", {}});

    auto result = ToolRegistry::instance().dispatch(
        "rl_select_environment", {{"name", "lunar_lander"}}, {});
    auto parsed = nlohmann::json::parse(result);
    EXPECT_TRUE(parsed.value("selected", false));

    auto req_body = nlohmann::json::parse(fake_->requests()[0].body);
    EXPECT_EQ(req_body["name"], "lunar_lander");
}

TEST_F(RlToolsTest, StartTrainingReturnsRunId) {
    fake_->enqueue_response(
        {200, R"({"run_id":"run-42","started":true})", {}});

    auto result = ToolRegistry::instance().dispatch(
        "rl_start_training", nlohmann::json::object(), {});
    auto parsed = nlohmann::json::parse(result);
    EXPECT_EQ(parsed["run_id"], "run-42");
    EXPECT_TRUE(parsed.value("started", false));
}

TEST_F(RlToolsTest, CheckStatusParsesRunStatus) {
    nlohmann::json body = {{"run_id", "run-42"},
                           {"state", "running"},
                           {"progress", 0.5},
                           {"metrics", {{"reward", 42.0}}}};
    fake_->enqueue_response({200, body.dump(), {}});

    auto result = ToolRegistry::instance().dispatch(
        "rl_check_status", {{"run_id", "run-42"}}, {});
    auto parsed = nlohmann::json::parse(result);
    EXPECT_EQ(parsed["state"], "running");
    EXPECT_DOUBLE_EQ(parsed["progress"].get<double>(), 0.5);
}

TEST_F(RlToolsTest, StopTrainingSendsCorrectUrl) {
    fake_->enqueue_response({200, R"({"stopped":true})", {}});

    auto result = ToolRegistry::instance().dispatch(
        "rl_stop_training", {{"run_id", "run-99"}}, {});
    auto parsed = nlohmann::json::parse(result);
    EXPECT_TRUE(parsed.value("stopped", false));
    EXPECT_NE(fake_->requests()[0].url.find("/training/run-99/stop"),
              std::string::npos);
}

TEST_F(RlToolsTest, MissingEnvVarsCheckFnFalse) {
    // Clear and re-register without env vars.
    ToolRegistry::instance().clear();
    unsetenv("NOUS_RL_API_URL");
    unsetenv("NOUS_RL_API_KEY");
    register_rl_tools(fake_.get());

    // Toolset check should fail.
    EXPECT_FALSE(
        ToolRegistry::instance().is_toolset_available("rl"));
}

TEST_F(RlToolsTest, TestInferenceSendsInput) {
    fake_->enqueue_response({200, R"({"output":"predicted_action"})", {}});

    auto result = ToolRegistry::instance().dispatch(
        "rl_test_inference",
        {{"run_id", "run-42"}, {"input", "observation_data"}},
        {});
    auto parsed = nlohmann::json::parse(result);
    EXPECT_EQ(parsed["output"], "predicted_action");

    auto req_body = nlohmann::json::parse(fake_->requests()[0].body);
    EXPECT_EQ(req_body["input"], "observation_data");
}

}  // namespace
