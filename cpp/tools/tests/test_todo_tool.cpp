#include "hermes/tools/todo_tool.hpp"
#include "hermes/tools/registry.hpp"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

using namespace hermes::tools;

namespace {

class TodoToolTest : public ::testing::Test {
protected:
    void SetUp() override {
        ToolRegistry::instance().clear();
        clear_all_todos();
        register_todo_tools(ToolRegistry::instance());
    }

    void TearDown() override {
        ToolRegistry::instance().clear();
        clear_all_todos();
    }

    std::string dispatch(const nlohmann::json& args) {
        return ToolRegistry::instance().dispatch("todo", args, ctx_);
    }

    ToolContext ctx_{"task1", "test", "s1", "cli", "/tmp", {}};
};

TEST_F(TodoToolTest, ReadEmpty) {
    auto r = nlohmann::json::parse(dispatch(nlohmann::json::object()));
    EXPECT_EQ(r["total"].get<int>(), 0);
    EXPECT_TRUE(r["todos"].is_array());
    EXPECT_TRUE(r["todos"].empty());
}

TEST_F(TodoToolTest, AddItems) {
    nlohmann::json args = {
        {"todos",
         {{{"content", "task A"}, {"status", "pending"}},
          {{"content", "task B"}, {"status", "in_progress"}}}}};
    auto r = nlohmann::json::parse(dispatch(args));
    EXPECT_EQ(r["total"].get<int>(), 2);
    // Items should have auto-generated ids
    EXPECT_FALSE(r["todos"][0]["id"].get<std::string>().empty());
}

TEST_F(TodoToolTest, MergeUpdate) {
    // Set initial
    nlohmann::json args1 = {
        {"todos",
         {{{"id", "t1"}, {"content", "do X"}, {"status", "pending"}},
          {{"id", "t2"}, {"content", "do Y"}, {"status", "pending"}}}}};
    dispatch(args1);

    // Merge: update t1, add t3
    nlohmann::json args2 = {
        {"todos",
         {{{"id", "t1"}, {"content", "do X revised"}, {"status", "completed"}},
          {{"id", "t3"}, {"content", "do Z"}, {"status", "pending"}}}},
        {"merge", true}};
    auto r = nlohmann::json::parse(dispatch(args2));
    EXPECT_EQ(r["total"].get<int>(), 3);

    // Verify t1 was updated
    bool found = false;
    for (const auto& t : r["todos"]) {
        if (t["id"] == "t1") {
            EXPECT_EQ(t["status"].get<std::string>(), "completed");
            EXPECT_EQ(t["content"].get<std::string>(), "do X revised");
            found = true;
        }
    }
    EXPECT_TRUE(found);
}

TEST_F(TodoToolTest, ReplaceList) {
    // Set initial list
    nlohmann::json args1 = {
        {"todos", {{{"id", "t1"}, {"content", "old"}, {"status", "pending"}}}}};
    dispatch(args1);

    // Replace with new list (merge=false is default)
    nlohmann::json args2 = {
        {"todos",
         {{{"id", "new1"}, {"content", "brand new"}, {"status", "pending"}}}}};
    auto r = nlohmann::json::parse(dispatch(args2));
    EXPECT_EQ(r["total"].get<int>(), 1);
    EXPECT_EQ(r["todos"][0]["id"].get<std::string>(), "new1");
}

TEST_F(TodoToolTest, InvalidStatusReturnsError) {
    nlohmann::json args = {
        {"todos",
         {{{"content", "bad"}, {"status", "exploded"}}}}};
    auto r = nlohmann::json::parse(dispatch(args));
    EXPECT_TRUE(r.contains("error"));
    EXPECT_NE(r["error"].get<std::string>().find("invalid status"),
              std::string::npos);
}

}  // namespace
