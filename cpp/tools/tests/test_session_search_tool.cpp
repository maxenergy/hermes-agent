#include "hermes/tools/session_search_tool.hpp"
#include "hermes/state/session_db.hpp"
#include "hermes/tools/registry.hpp"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <cstdlib>
#include <filesystem>
#include <random>
#include <sstream>

using namespace hermes::tools;
namespace fs = std::filesystem;

namespace {

class SessionSearchToolTest : public ::testing::Test {
protected:
    void SetUp() override {
        ToolRegistry::instance().clear();
        std::random_device rd;
        std::mt19937 gen(rd());
        std::ostringstream oss;
        oss << "hermes_search_test_" << gen();
        tmp_ = fs::temp_directory_path() / oss.str();
        fs::create_directories(tmp_);
        setenv("HERMES_HOME", tmp_.c_str(), 1);
        register_session_search_tools(ToolRegistry::instance());
    }

    void TearDown() override {
        ToolRegistry::instance().clear();
        fs::remove_all(tmp_);
        unsetenv("HERMES_HOME");
    }

    std::string dispatch(const nlohmann::json& args) {
        return ToolRegistry::instance().dispatch("session_search", args, ctx_);
    }

    ToolContext ctx_{"task1", "test", "s1", "cli", "/tmp", {}};
    fs::path tmp_;
};

TEST_F(SessionSearchToolTest, SearchReturnsResults) {
    // Insert some data into the DB
    hermes::state::SessionDB db;
    auto sid = db.create_session("cli", "gpt-4", {});
    hermes::state::MessageRow msg;
    msg.session_id = sid;
    msg.turn_index = 0;
    msg.role = "user";
    msg.content = "How do I configure the frobnicator?";
    db.save_message(msg);

    auto r = nlohmann::json::parse(
        dispatch({{"query", "frobnicator"}}));
    EXPECT_TRUE(r.contains("results"));
    EXPECT_TRUE(r.contains("count"));
    // FTS should find our message
    EXPECT_GE(r["count"].get<int>(), 1);
}

TEST_F(SessionSearchToolTest, EmptyQueryReturnsEmpty) {
    auto r = nlohmann::json::parse(dispatch({{"query", ""}}));
    EXPECT_EQ(r["count"].get<int>(), 0);
    EXPECT_TRUE(r["results"].empty());
}

}  // namespace
