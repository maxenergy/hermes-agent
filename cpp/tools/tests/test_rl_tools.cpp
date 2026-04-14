#include "hermes/llm/llm_client.hpp"
#include "hermes/tools/registry.hpp"
#include "hermes/tools/rl_training_tool.hpp"

#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>

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

TEST_F(RlToolsTest, LockedFieldsExposesExpectedKeys) {
    auto j = hermes::tools::locked_fields();
    ASSERT_TRUE(j.contains("env"));
    ASSERT_TRUE(j["env"].contains("tokenizer_name"));
    EXPECT_EQ(j["env"]["tokenizer_name"], "Qwen/Qwen3-8B");
    EXPECT_TRUE(j["env"]["use_wandb"].get<bool>());
    ASSERT_TRUE(j.contains("openai"));
    ASSERT_TRUE(j["openai"].is_array());
    ASSERT_FALSE(j["openai"].empty());
    EXPECT_EQ(j["openai"][0]["server_type"], "sglang");
    ASSERT_TRUE(j.contains("tinker"));
    EXPECT_EQ(j["tinker"]["lora_rank"].get<int>(), 32);
}

TEST_F(RlToolsTest, FirstMissingEnvReturnsName) {
    unsetenv("__HERMES_TEST_ENV_ONE__");
    unsetenv("__HERMES_TEST_ENV_TWO__");
    EXPECT_EQ(hermes::tools::first_missing_env({"__HERMES_TEST_ENV_ONE__"}),
              "__HERMES_TEST_ENV_ONE__");
    setenv("__HERMES_TEST_ENV_ONE__", "value", 1);
    EXPECT_EQ(hermes::tools::first_missing_env({"__HERMES_TEST_ENV_ONE__"}),
              std::string());
    EXPECT_EQ(hermes::tools::first_missing_env(
                  {"__HERMES_TEST_ENV_ONE__", "__HERMES_TEST_ENV_TWO__"}),
              "__HERMES_TEST_ENV_TWO__");
    unsetenv("__HERMES_TEST_ENV_ONE__");
}

TEST_F(RlToolsTest, BuildRunYamlIncludesLockedAndUserFields) {
    nlohmann::json user = {
        {"dataset_name", "hf/widgets"},
        {"wandb_name", "widgets-alpha"},
    };
    auto yaml = hermes::tools::build_run_yaml("widgets", user, "proj",
                                              "widgets-run-1");
    EXPECT_NE(yaml.find("tokenizer_name"), std::string::npos);
    EXPECT_NE(yaml.find("dataset_name"), std::string::npos);
    EXPECT_NE(yaml.find("hf/widgets"), std::string::npos);
    EXPECT_NE(yaml.find("wandb_project"), std::string::npos);
    EXPECT_NE(yaml.find("widgets-run-1"), std::string::npos);
}

TEST_F(RlToolsTest, ParseEnvFileRecognizesBaseEnvSubclass) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path() / "hermes_rl_env_test";
    fs::create_directories(dir);
    auto file = dir / "cartpole.py";
    {
        std::ofstream ofs(file);
        ofs << "from atroposlib.envs import BaseEnv, BaseEnvConfig\n\n"
               "class CartpoleEnv(BaseEnv):\n"
               "    \"\"\"Classic cartpole balancing task.\n\nExtended docs.\"\"\"\n"
               "    name = \"cartpole_v2\"\n"
               "    env_config_cls = CartpoleConfig\n"
               "    def __init__(self):\n"
               "        pass\n";
    }
    auto info = hermes::tools::parse_env_file(file.string());
    ASSERT_TRUE(info.has_value());
    EXPECT_EQ(info->class_name, "CartpoleEnv");
    EXPECT_EQ(info->name, "cartpole_v2");
    EXPECT_EQ(info->config_class, "CartpoleConfig");
    EXPECT_NE(info->description.find("cartpole"), std::string::npos);
    fs::remove_all(dir);
}

TEST_F(RlToolsTest, ParseEnvFileRejectsNonBaseEnv) {
    namespace fs = std::filesystem;
    auto file = fs::temp_directory_path() / "hermes_rl_plain.py";
    {
        std::ofstream ofs(file);
        ofs << "class Plain:\n    pass\n";
    }
    auto info = hermes::tools::parse_env_file(file.string());
    EXPECT_FALSE(info.has_value());
    fs::remove(file);
}

TEST_F(RlToolsTest, LocalRegistryTracksSelectedEnvironment) {
    auto& local = hermes::tools::RlLocalRegistry::instance();
    local.reset();
    local.set_current_env("demo");
    ASSERT_TRUE(local.current_env().has_value());
    EXPECT_EQ(*local.current_env(), "demo");
    auto cfg = local.current_config();
    EXPECT_TRUE(cfg.contains("wandb_name"));
    local.set_current_config_field("temperature", 0.7);
    EXPECT_DOUBLE_EQ(local.current_config().at("temperature").get<double>(),
                     0.7);
    local.reset();
    EXPECT_FALSE(local.current_env().has_value());
}

TEST_F(RlToolsTest, LocalRegistryCreateRunAssignsLogs) {
    auto& local = hermes::tools::RlLocalRegistry::instance();
    local.reset();
    auto* run = local.create_run("demo", nlohmann::json::object());
    ASSERT_NE(run, nullptr);
    EXPECT_FALSE(run->run_id.empty());
    EXPECT_EQ(run->environment, "demo");
    EXPECT_EQ(run->status, "starting");
    EXPECT_NE(run->api_log_path.find(run->run_id), std::string::npos);
    EXPECT_NE(run->trainer_log_path.find(run->run_id), std::string::npos);
    EXPECT_NE(run->env_log_path.find(run->run_id), std::string::npos);
    auto* again = local.get_run(run->run_id);
    EXPECT_EQ(again, run);
    local.set_run_status(run->run_id, "completed");
    EXPECT_EQ(local.get_run(run->run_id)->status, "completed");
    auto all = local.list_runs();
    EXPECT_EQ(all.size(), 1u);
    local.reset();
}

TEST_F(RlToolsTest, LocalRegistryStatusCheckRateLimiting) {
    auto& local = hermes::tools::RlLocalRegistry::instance();
    local.reset();
    EXPECT_EQ(local.status_check_cooldown("r1"), 0);
    local.record_status_check("r1");
    auto remaining = local.status_check_cooldown("r1");
    EXPECT_GT(remaining, 0);
    EXPECT_LE(remaining, hermes::tools::kMinStatusCheckIntervalSeconds);
    local.reset();
}

TEST_F(RlToolsTest, LocalRegistryEnvConfigFieldsHasLockedFlags) {
    auto& local = hermes::tools::RlLocalRegistry::instance();
    local.reset();
    auto fields = local.env_config_fields("demo");
    ASSERT_TRUE(fields.contains("tokenizer_name"));
    EXPECT_TRUE(fields["tokenizer_name"]["locked"].get<bool>());
    ASSERT_TRUE(fields.contains("dataset_name"));
    EXPECT_FALSE(fields["dataset_name"]["locked"].get<bool>());
}

TEST_F(RlToolsTest, LocalModeEditConfigRejectsLockedFields) {
    // Drop HTTP creds; keep TINKER_ATROPOS_ROOT unset so HTTP path is used
    // only because NOUS_* is set.  We instead point HTTP at a dead url,
    // but explicitly route via local path by clearing HTTP creds.
    unsetenv("NOUS_RL_API_URL");
    unsetenv("NOUS_RL_API_KEY");
    // Make toolset check pass via local root.
    namespace fs = std::filesystem;
    auto root = fs::temp_directory_path() / "hermes_rl_root";
    fs::create_directories(root / "tinker_atropos" / "environments");
    setenv("TINKER_ATROPOS_ROOT", root.string().c_str(), 1);

    ToolRegistry::instance().clear();
    register_rl_tools(nullptr);

    auto& local = hermes::tools::RlLocalRegistry::instance();
    local.reset();
    local.set_current_env("demo");

    auto result = ToolRegistry::instance().dispatch(
        "rl_edit_config", {{"key", "tokenizer_name"}, {"value", "foo"}}, {});
    auto parsed = nlohmann::json::parse(result);
    ASSERT_TRUE(parsed.contains("error"));
    EXPECT_NE(parsed["error"].get<std::string>().find("locked"),
              std::string::npos);

    fs::remove_all(root);
    unsetenv("TINKER_ATROPOS_ROOT");
}

TEST_F(RlToolsTest, LocalModeStartTrainingRequiresSelectedEnv) {
    unsetenv("NOUS_RL_API_URL");
    unsetenv("NOUS_RL_API_KEY");
    namespace fs = std::filesystem;
    auto root = fs::temp_directory_path() / "hermes_rl_root2";
    fs::create_directories(root / "tinker_atropos" / "environments");
    setenv("TINKER_ATROPOS_ROOT", root.string().c_str(), 1);

    ToolRegistry::instance().clear();
    register_rl_tools(nullptr);
    hermes::tools::RlLocalRegistry::instance().reset();

    auto result = ToolRegistry::instance().dispatch(
        "rl_start_training", nlohmann::json::object(), {});
    auto parsed = nlohmann::json::parse(result);
    EXPECT_TRUE(parsed.contains("error"));
    EXPECT_NE(parsed["error"].get<std::string>().find("No environment"),
              std::string::npos);

    fs::remove_all(root);
    unsetenv("TINKER_ATROPOS_ROOT");
}

TEST_F(RlToolsTest, TestModelsReturnsThreeScales) {
    const auto& models = hermes::tools::RlLocalRegistry::test_models();
    ASSERT_EQ(models.size(), 3u);
    EXPECT_EQ(models[0].scale, "small");
    EXPECT_EQ(models[1].scale, "medium");
    EXPECT_EQ(models[2].scale, "large");
}

}  // namespace
