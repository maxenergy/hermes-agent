#include "hermes/tools/memory_tool.hpp"
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

class MemoryToolTest : public ::testing::Test {
protected:
    void SetUp() override {
        ToolRegistry::instance().clear();

        // Use a unique temp dir so parallel CTest invocations don't race.
        std::random_device rd;
        std::mt19937 gen(rd());
        std::ostringstream oss;
        oss << "hermes_mem_test_" << gen();
        tmp_ = fs::temp_directory_path() / oss.str();
        fs::create_directories(tmp_);
        setenv("HERMES_HOME", tmp_.c_str(), 1);

        register_memory_tools(ToolRegistry::instance());
    }

    void TearDown() override {
        ToolRegistry::instance().clear();
        fs::remove_all(tmp_);
        unsetenv("HERMES_HOME");
    }

    std::string dispatch(const nlohmann::json& args) {
        return ToolRegistry::instance().dispatch("memory", args, ctx_);
    }

    ToolContext ctx_{"task1", "test", "s1", "cli", "/tmp", {}};
    fs::path tmp_;
};

TEST_F(MemoryToolTest, AddAndReadRoundTrip) {
    auto r = nlohmann::json::parse(
        dispatch({{"action", "add"}, {"entry", "remember this"}}));
    EXPECT_TRUE(r["added"].get<bool>());
    EXPECT_EQ(r["count"].get<int>(), 1);

    auto r2 = nlohmann::json::parse(dispatch({{"action", "read"}}));
    EXPECT_EQ(r2["count"].get<int>(), 1);
    EXPECT_EQ(r2["entries"][0].get<std::string>(), "remember this");
}

TEST_F(MemoryToolTest, Replace) {
    dispatch({{"action", "add"}, {"entry", "old value here"}});
    auto r = nlohmann::json::parse(
        dispatch({{"action", "replace"},
                  {"needle", "old value"},
                  {"replacement", "new value here"}}));
    EXPECT_TRUE(r["replaced"].get<bool>());

    auto r2 = nlohmann::json::parse(dispatch({{"action", "read"}}));
    EXPECT_EQ(r2["entries"][0].get<std::string>(), "new value here");
}

TEST_F(MemoryToolTest, Remove) {
    dispatch({{"action", "add"}, {"entry", "delete me"}});
    dispatch({{"action", "add"}, {"entry", "keep me"}});

    auto r = nlohmann::json::parse(
        dispatch({{"action", "remove"}, {"needle", "delete me"}}));
    EXPECT_TRUE(r["removed"].get<bool>());

    auto r2 = nlohmann::json::parse(dispatch({{"action", "read"}}));
    EXPECT_EQ(r2["count"].get<int>(), 1);
    EXPECT_EQ(r2["entries"][0].get<std::string>(), "keep me");
}

TEST_F(MemoryToolTest, InvalidActionReturnsError) {
    auto r = nlohmann::json::parse(dispatch({{"action", "explode"}}));
    EXPECT_TRUE(r.contains("error"));
    EXPECT_NE(r["error"].get<std::string>().find("invalid action"),
              std::string::npos);
}

}  // namespace
