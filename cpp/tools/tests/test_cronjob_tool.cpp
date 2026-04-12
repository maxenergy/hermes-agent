#include "hermes/tools/cronjob_tool.hpp"
#include "hermes/tools/registry.hpp"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <string>

using namespace hermes::tools;
using json = nlohmann::json;

namespace {

class CronjobToolTest : public ::testing::Test {
protected:
    void SetUp() override {
        ToolRegistry::instance().clear();
        register_cronjob_tools(ToolRegistry::instance());
    }

    void TearDown() override {
        ToolRegistry::instance().clear();
    }

    std::string dispatch(const std::string& tool, const json& args) {
        return ToolRegistry::instance().dispatch(tool, args, ctx_);
    }

    ToolContext ctx_{"task1", "test", "s1", "cli", "/tmp", {}};
};

TEST_F(CronjobToolTest, CreateAndList) {
    auto cr = json::parse(dispatch("cronjob",
        {{"action", "create"},
         {"name", "daily-report"},
         {"schedule", "0 9 * * *"},
         {"prompt", "Generate daily report"}}));
    EXPECT_TRUE(cr["created"].get<bool>());
    EXPECT_FALSE(cr["job_id"].get<std::string>().empty());

    auto lr = json::parse(dispatch("cronjob", {{"action", "list"}}));
    EXPECT_EQ(lr["count"].get<int>(), 1);
    EXPECT_EQ(lr["jobs"][0]["name"].get<std::string>(), "daily-report");
}

TEST_F(CronjobToolTest, RunTriggers) {
    auto cr = json::parse(dispatch("cronjob",
        {{"action", "create"},
         {"name", "test-job"},
         {"schedule", "*/5 * * * *"},
         {"prompt", "do stuff"}}));
    auto job_id = cr["job_id"].get<std::string>();

    auto rr = json::parse(dispatch("cronjob",
        {{"action", "run"}, {"job_id", job_id}}));
    EXPECT_TRUE(rr["triggered"].get<bool>());
}

TEST_F(CronjobToolTest, PauseAndResume) {
    auto cr = json::parse(dispatch("cronjob",
        {{"action", "create"},
         {"name", "pausable"},
         {"schedule", "0 * * * *"},
         {"prompt", "check status"}}));
    auto job_id = cr["job_id"].get<std::string>();

    auto pr = json::parse(dispatch("cronjob",
        {{"action", "pause"}, {"job_id", job_id}}));
    EXPECT_TRUE(pr["paused"].get<bool>());

    // List should show paused state.
    auto lr = json::parse(dispatch("cronjob", {{"action", "list"}}));
    EXPECT_TRUE(lr["jobs"][0]["paused"].get<bool>());

    auto rr = json::parse(dispatch("cronjob",
        {{"action", "resume"}, {"job_id", job_id}}));
    EXPECT_TRUE(rr["resumed"].get<bool>());
}

TEST_F(CronjobToolTest, Delete) {
    auto cr = json::parse(dispatch("cronjob",
        {{"action", "create"},
         {"name", "deletable"},
         {"schedule", "30 2 * * 1"},
         {"prompt", "weekly task"}}));
    auto job_id = cr["job_id"].get<std::string>();

    auto dr = json::parse(dispatch("cronjob",
        {{"action", "delete"}, {"job_id", job_id}}));
    EXPECT_TRUE(dr["deleted"].get<bool>());

    // List should be empty.
    auto lr = json::parse(dispatch("cronjob", {{"action", "list"}}));
    EXPECT_EQ(lr["count"].get<int>(), 0);
}

TEST_F(CronjobToolTest, InvalidCronExpression) {
    auto r = json::parse(dispatch("cronjob",
        {{"action", "create"},
         {"name", "bad-cron"},
         {"schedule", "not a cron"},
         {"prompt", "will fail"}}));
    EXPECT_TRUE(r.contains("error"));
    EXPECT_NE(r["error"].get<std::string>().find("invalid cron"),
              std::string::npos);
}

TEST_F(CronjobToolTest, ParseCronValidExpressions) {
    EXPECT_TRUE(parse_cron_expression("* * * * *"));
    EXPECT_TRUE(parse_cron_expression("*/5 * * * *"));
    EXPECT_TRUE(parse_cron_expression("0 9 * * *"));
    EXPECT_TRUE(parse_cron_expression("30 2 1-15 * 1"));
    EXPECT_FALSE(parse_cron_expression("bad"));
    EXPECT_FALSE(parse_cron_expression("* * *"));  // only 3 fields
    EXPECT_FALSE(parse_cron_expression("a b c d e"));
}

}  // namespace
