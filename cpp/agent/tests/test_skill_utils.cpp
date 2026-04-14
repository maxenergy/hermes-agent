// Unit tests for hermes::agent::skill_utils — C++17 port of
// agent/skill_utils.py.
#include "hermes/agent/skill_utils.hpp"

#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

using namespace hermes::agent::skill_utils;
namespace fs = std::filesystem;

namespace {

class TempHome : public ::testing::Test {
protected:
    fs::path tmp;
    std::string saved_hermes_home;
    std::string saved_home;
    std::string saved_platform;
    std::string saved_session_platform;

    void SetUp() override {
        auto base = fs::temp_directory_path() / "hermes_skill_utils_test";
        fs::create_directories(base);
        tmp = fs::path(base) / ("t" + std::to_string(::getpid()) + "_" +
                                std::to_string(::testing::UnitTest::GetInstance()
                                                   ->current_test_info()
                                                   ->name()[0]));
        fs::create_directories(tmp);
        saved_hermes_home = getenv("HERMES_HOME") ? getenv("HERMES_HOME") : "";
        saved_home = getenv("HOME") ? getenv("HOME") : "";
        saved_platform = getenv("HERMES_PLATFORM") ? getenv("HERMES_PLATFORM") : "";
        saved_session_platform =
            getenv("HERMES_SESSION_PLATFORM") ? getenv("HERMES_SESSION_PLATFORM") : "";
        setenv("HERMES_HOME", tmp.c_str(), 1);
        unsetenv("HERMES_PLATFORM");
        unsetenv("HERMES_SESSION_PLATFORM");
    }

    void TearDown() override {
        std::error_code ec;
        fs::remove_all(tmp, ec);
        auto restore = [](const char* k, const std::string& v) {
            if (v.empty()) unsetenv(k);
            else setenv(k, v.c_str(), 1);
        };
        restore("HERMES_HOME", saved_hermes_home);
        restore("HOME", saved_home);
        restore("HERMES_PLATFORM", saved_platform);
        restore("HERMES_SESSION_PLATFORM", saved_session_platform);
    }

    void write(const fs::path& p, const std::string& content) {
        fs::create_directories(p.parent_path());
        std::ofstream ofs(p);
        ofs << content;
    }
};

TEST_F(TempHome, ParseFrontmatterEmpty) {
    auto fm = parse_frontmatter("no frontmatter here");
    EXPECT_TRUE(fm.data.empty());
    EXPECT_EQ(fm.body, "no frontmatter here");
}

TEST_F(TempHome, ParseFrontmatterSimpleYaml) {
    std::string s = "---\nname: foo\ndescription: A test\n---\nbody text";
    auto fm = parse_frontmatter(s);
    EXPECT_EQ(fm.data["name"], "foo");
    EXPECT_EQ(fm.data["description"], "A test");
    EXPECT_EQ(fm.body, "body text");
}

TEST_F(TempHome, ParseFrontmatterNoClose) {
    auto fm = parse_frontmatter("---\nname: foo\n(no close)");
    EXPECT_TRUE(fm.data.empty());
}

TEST_F(TempHome, NormalizePlatformName) {
    EXPECT_EQ(normalize_platform_name("macOS"), "darwin");
    EXPECT_EQ(normalize_platform_name("Linux"), "linux");
    EXPECT_EQ(normalize_platform_name("Windows"), "win32");
    EXPECT_EQ(normalize_platform_name("freebsd"), "freebsd");
}

TEST_F(TempHome, SkillMatchesPlatformMissingAllowsAll) {
    nlohmann::json fm = {{"name", "x"}};
    EXPECT_TRUE(skill_matches_platform(fm));
}

TEST_F(TempHome, SkillMatchesPlatformExplicit) {
    nlohmann::json fm;
    fm["platforms"] = nlohmann::json::array({current_platform_id() == "darwin"
                                                 ? "macos"
                                             : current_platform_id() == "win32"
                                                 ? "windows"
                                                 : "linux"});
    EXPECT_TRUE(skill_matches_platform(fm));

    nlohmann::json fm2;
    fm2["platforms"] = nlohmann::json::array({"someotheros"});
    EXPECT_FALSE(skill_matches_platform(fm2));
}

TEST_F(TempHome, NormalizeStringSetVariants) {
    EXPECT_TRUE(normalize_string_set(nlohmann::json()).empty());
    auto s1 = normalize_string_set(nlohmann::json("single"));
    EXPECT_EQ(s1.size(), 1u);
    auto s2 = normalize_string_set(nlohmann::json::array({"a", "  b  ", "", "c"}));
    EXPECT_EQ(s2.count("a"), 1u);
    EXPECT_EQ(s2.count("b"), 1u);
    EXPECT_EQ(s2.count("c"), 1u);
    EXPECT_EQ(s2.size(), 3u);
}

TEST_F(TempHome, DisabledSkillsFromConfig) {
    write(tmp / "config.yaml",
          "skills:\n  disabled:\n    - foo\n    - bar\n");
    auto d = get_disabled_skill_names();
    EXPECT_EQ(d.count("foo"), 1u);
    EXPECT_EQ(d.count("bar"), 1u);
}

TEST_F(TempHome, DisabledSkillsPlatformOverride) {
    write(tmp / "config.yaml",
          "skills:\n  disabled: [global]\n  platform_disabled:\n"
          "    telegram: [tg_only]\n");
    auto d = get_disabled_skill_names("telegram");
    EXPECT_EQ(d.count("tg_only"), 1u);
    EXPECT_EQ(d.count("global"), 0u);
}

TEST_F(TempHome, ExtractSkillDescription) {
    nlohmann::json fm;
    fm["description"] = "Short desc";
    EXPECT_EQ(extract_skill_description(fm), "Short desc");

    fm["description"] =
        std::string(100, 'x');  // 100 chars
    auto trimmed = extract_skill_description(fm);
    EXPECT_EQ(trimmed.size(), 60u);
    EXPECT_TRUE(trimmed.find("...") != std::string::npos);

    fm["description"] = "'quoted'";
    EXPECT_EQ(extract_skill_description(fm), "quoted");

    nlohmann::json fm2;
    EXPECT_EQ(extract_skill_description(fm2), "");
}

TEST_F(TempHome, ExpandPathHomeAndVars) {
    setenv("HOME", "/home/u", 1);
    setenv("MYVAR", "hello", 1);
    EXPECT_EQ(expand_path("~/x"), "/home/u/x");
    EXPECT_EQ(expand_path("${MYVAR}/y"), "hello/y");
    EXPECT_EQ(expand_path("plain"), "plain");
}

TEST_F(TempHome, ExtractSkillConditions) {
    auto fm = nlohmann::json::parse(R"({
        "metadata": {"hermes": {
            "requires_toolsets": ["dev", "web"],
            "fallback_for_tools": "single_as_string"
        }}
    })");
    auto c = extract_skill_conditions(fm);
    ASSERT_EQ(c.requires_toolsets.size(), 2u);
    EXPECT_EQ(c.requires_toolsets[0], "dev");
    EXPECT_EQ(c.fallback_for_tools.size(), 1u);
    EXPECT_EQ(c.fallback_for_tools[0], "single_as_string");
    EXPECT_TRUE(c.requires_tools.empty());
}

TEST_F(TempHome, ExtractSkillConfigVars) {
    auto fm = nlohmann::json::parse(R"({
        "metadata": {"hermes": {"config": [
            {"key": "wiki.path", "description": "Wiki dir", "default": "~/w"},
            {"key": "other", "description": "Needs prompt", "prompt": "Please"},
            {"key": "", "description": "skipped"},
            {"key": "nodesc"},
            {"key": "wiki.path", "description": "dup"}
        ]}}
    })");
    auto vars = extract_skill_config_vars(fm);
    ASSERT_EQ(vars.size(), 2u);
    EXPECT_EQ(vars[0].key, "wiki.path");
    ASSERT_TRUE(vars[0].default_value.has_value());
    EXPECT_EQ(vars[0].default_value->get<std::string>(), "~/w");
    EXPECT_EQ(vars[0].prompt, "Wiki dir");  // defaults to description
    EXPECT_EQ(vars[1].prompt, "Please");
}

TEST_F(TempHome, ResolveDotpath) {
    auto j = nlohmann::json::parse(R"({"a":{"b":{"c":42}}})");
    auto* v = resolve_dotpath(j, "a.b.c");
    ASSERT_TRUE(v != nullptr);
    EXPECT_EQ(v->get<int>(), 42);
    EXPECT_EQ(resolve_dotpath(j, "a.x.c"), nullptr);
    EXPECT_EQ(resolve_dotpath(j, "a"), &j["a"]);
}

TEST_F(TempHome, ResolveSkillConfigValuesUsesDefaults) {
    write(tmp / "config.yaml", "skills:\n  config:\n    wiki:\n      path: /custom\n");
    std::vector<SkillConfigVar> vars;
    SkillConfigVar v;
    v.key = "wiki.path";
    v.description = "Wiki";
    v.prompt = "Wiki?";
    v.default_value = nlohmann::json("~/default");
    vars.push_back(v);

    SkillConfigVar v2;
    v2.key = "missing.path";
    v2.description = "Missing";
    v2.prompt = "?";
    v2.default_value = nlohmann::json("~/fallback");
    vars.push_back(v2);

    setenv("HOME", "/home/user", 1);
    auto resolved = resolve_skill_config_values(vars);
    EXPECT_EQ(resolved["wiki.path"], "/custom");
    EXPECT_EQ(resolved["missing.path"], "/home/user/fallback");
}

TEST_F(TempHome, IterSkillIndexFilesSortedAndExcludes) {
    write(tmp / "skills" / "a" / "SKILL.md", "---\nname: a\n---\n");
    write(tmp / "skills" / "b" / "SKILL.md", "---\nname: b\n---\n");
    write(tmp / "skills" / ".git" / "SKILL.md", "---\nname: git\n---\n");
    auto files = iter_skill_index_files(tmp / "skills", "SKILL.md");
    ASSERT_EQ(files.size(), 2u);
    EXPECT_EQ(files[0].parent_path().filename(), "a");
    EXPECT_EQ(files[1].parent_path().filename(), "b");
}

TEST_F(TempHome, GetExternalSkillsDirsExpands) {
    auto ext = tmp / "ext_skills";
    fs::create_directories(ext);
    write(tmp / "config.yaml",
          std::string("skills:\n  external_dirs:\n    - ") + ext.string() +
              "\n");
    auto dirs = get_external_skills_dirs();
    ASSERT_EQ(dirs.size(), 1u);
    EXPECT_EQ(fs::weakly_canonical(dirs[0]), fs::weakly_canonical(ext));
}

TEST_F(TempHome, GetAllSkillsDirsIncludesLocalFirst) {
    auto dirs = get_all_skills_dirs();
    ASSERT_FALSE(dirs.empty());
    EXPECT_EQ(dirs[0].filename(), "skills");
}

TEST_F(TempHome, DiscoverAllSkillConfigVarsDedup) {
    auto skills = tmp / "skills";
    write(skills / "a" / "SKILL.md",
          "---\nname: a\nmetadata:\n  hermes:\n    config:\n"
          "      - key: shared\n        description: From A\n"
          "---\nbody\n");
    write(skills / "b" / "SKILL.md",
          "---\nname: b\nmetadata:\n  hermes:\n    config:\n"
          "      - key: shared\n        description: From B\n"
          "      - key: unique_b\n        description: Only B\n"
          "---\nbody\n");
    auto vars = discover_all_skill_config_vars();
    EXPECT_EQ(vars.size(), 2u);
    bool saw_a = false, saw_u = false;
    for (const auto& v : vars) {
        if (v.key == "shared") {
            EXPECT_EQ(v.skill, "a");  // first one wins
            saw_a = true;
        }
        if (v.key == "unique_b") {
            EXPECT_EQ(v.skill, "b");
            saw_u = true;
        }
    }
    EXPECT_TRUE(saw_a && saw_u);
}

}  // namespace
