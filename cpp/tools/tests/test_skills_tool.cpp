#include "hermes/tools/skills_tool.hpp"
#include "hermes/tools/registry.hpp"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <random>
#include <sstream>

using namespace hermes::tools;
namespace fs = std::filesystem;

namespace {

class SkillsToolTest : public ::testing::Test {
protected:
    void SetUp() override {
        ToolRegistry::instance().clear();
        std::random_device rd;
        std::mt19937 gen(rd());
        std::ostringstream oss;
        oss << "hermes_skills_test_" << gen();
        tmp_ = fs::temp_directory_path() / oss.str();
        fs::create_directories(tmp_);
        setenv("HERMES_HOME", tmp_.c_str(), 1);
        register_skills_tools(ToolRegistry::instance());
    }

    void TearDown() override {
        ToolRegistry::instance().clear();
        fs::remove_all(tmp_);
        unsetenv("HERMES_HOME");
    }

    std::string dispatch(const std::string& tool, const nlohmann::json& args) {
        return ToolRegistry::instance().dispatch(tool, args, ctx_);
    }

    ToolContext ctx_{"task1", "test", "s1", "cli", "/tmp", {}};
    fs::path tmp_;
};

TEST_F(SkillsToolTest, EmptyDirReturnsEmptyList) {
    auto r = nlohmann::json::parse(
        dispatch("skills_list", nlohmann::json::object()));
    EXPECT_EQ(r["count"].get<int>(), 0);
    EXPECT_TRUE(r["skills"].is_array());
    EXPECT_TRUE(r["skills"].empty());
}

TEST_F(SkillsToolTest, CreateMockSkillFound) {
    auto skill_dir = tmp_ / "skills" / "my-skill";
    fs::create_directories(skill_dir);
    {
        std::ofstream ofs(skill_dir / "index.json");
        ofs << R"({"description": "A test skill"})";
    }
    {
        std::ofstream ofs(skill_dir / "SKILL.md");
        ofs << "# My Skill\nDoes things.";
    }

    auto r = nlohmann::json::parse(
        dispatch("skills_list", nlohmann::json::object()));
    EXPECT_EQ(r["count"].get<int>(), 1);
    EXPECT_EQ(r["skills"][0]["name"].get<std::string>(), "my-skill");
    EXPECT_EQ(r["skills"][0]["description"].get<std::string>(), "A test skill");
}

TEST_F(SkillsToolTest, SkillViewReadsContent) {
    auto skill_dir = tmp_ / "skills" / "viewer-test";
    fs::create_directories(skill_dir);
    {
        std::ofstream ofs(skill_dir / "SKILL.md");
        ofs << "# Viewer\nHello world.";
    }

    auto r = nlohmann::json::parse(
        dispatch("skill_view", {{"name", "viewer-test"}}));
    EXPECT_EQ(r["name"].get<std::string>(), "viewer-test");
    EXPECT_NE(r["content"].get<std::string>().find("Hello world"),
              std::string::npos);
}

}  // namespace
