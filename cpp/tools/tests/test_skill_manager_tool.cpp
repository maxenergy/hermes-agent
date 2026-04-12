#include "hermes/tools/skill_manager_tool.hpp"
#include "hermes/tools/registry.hpp"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <random>
#include <sstream>

using namespace hermes::tools;
using json = nlohmann::json;
namespace fs = std::filesystem;

namespace {

class SkillManagerToolTest : public ::testing::Test {
protected:
    void SetUp() override {
        ToolRegistry::instance().clear();
        std::random_device rd;
        std::mt19937 gen(rd());
        std::ostringstream oss;
        oss << "hermes_skill_mgr_test_" << gen();
        tmp_ = fs::temp_directory_path() / oss.str();
        fs::create_directories(tmp_);
        setenv("HERMES_HOME", tmp_.c_str(), 1);
        register_skill_manager_tools(ToolRegistry::instance());
    }

    void TearDown() override {
        ToolRegistry::instance().clear();
        fs::remove_all(tmp_);
        unsetenv("HERMES_HOME");
    }

    std::string dispatch(const std::string& tool, const json& args) {
        return ToolRegistry::instance().dispatch(tool, args, ctx_);
    }

    ToolContext ctx_{"task1", "test", "s1", "cli", "/tmp", {}};
    fs::path tmp_;
};

TEST_F(SkillManagerToolTest, ListInstalledEmpty) {
    auto r = json::parse(dispatch("skill_manage",
        {{"action", "list_installed"}}));
    EXPECT_EQ(r["count"].get<int>(), 0);
    EXPECT_TRUE(r["skills"].is_array());
    EXPECT_TRUE(r["skills"].empty());
}

TEST_F(SkillManagerToolTest, UninstallSafetyCheck) {
    // Path traversal attack should be rejected.
    auto r1 = json::parse(dispatch("skill_manage",
        {{"action", "uninstall"}, {"name", "../escape"}}));
    EXPECT_TRUE(r1.contains("error"));

    auto r2 = json::parse(dispatch("skill_manage",
        {{"action", "uninstall"}, {"name", ".."}}));
    EXPECT_TRUE(r2.contains("error"));

    auto r3 = json::parse(dispatch("skill_manage",
        {{"action", "uninstall"}, {"name", "nonexistent"}}));
    EXPECT_TRUE(r3.contains("error"));
}

TEST_F(SkillManagerToolTest, SearchStubReturnsError) {
    auto r = json::parse(dispatch("skill_manage",
        {{"action", "search"}, {"query", "something"}}));
    EXPECT_TRUE(r.contains("error"));
    EXPECT_NE(r["error"].get<std::string>().find("Hub not connected"),
              std::string::npos);
}

TEST_F(SkillManagerToolTest, UninstallExistingSkill) {
    // Create a skill directory.
    auto skill_dir = tmp_ / "skills" / "test-skill";
    fs::create_directories(skill_dir);
    {
        std::ofstream ofs(skill_dir / "SKILL.md");
        ofs << "# Test Skill";
    }

    auto r = json::parse(dispatch("skill_manage",
        {{"action", "uninstall"}, {"name", "test-skill"}}));
    EXPECT_TRUE(r["uninstalled"].get<bool>());
    EXPECT_FALSE(fs::exists(skill_dir));
}

}  // namespace
