// Tests for hermes/tools/skills_tool_depth.hpp.
#include "hermes/tools/skills_tool_depth.hpp"

#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

using namespace hermes::tools::skills_depth;
using json = nlohmann::json;

TEST(SkillsDepthReadiness, ParseNames) {
    EXPECT_EQ(parse_readiness("ready"), ReadinessStatus::Ready);
    EXPECT_EQ(parse_readiness("READY"), ReadinessStatus::Ready);
    EXPECT_EQ(parse_readiness("missing_env"), ReadinessStatus::MissingEnv);
    EXPECT_EQ(parse_readiness("missing-env"), ReadinessStatus::MissingEnv);
    EXPECT_EQ(parse_readiness("missing_command"),
              ReadinessStatus::MissingCommand);
    EXPECT_EQ(parse_readiness("incompatible"), ReadinessStatus::Incompatible);
    EXPECT_EQ(parse_readiness("bogus"), ReadinessStatus::Unknown);
    EXPECT_EQ(parse_readiness(""), ReadinessStatus::Unknown);
}

TEST(SkillsDepthReadiness, NameRoundTrip) {
    EXPECT_EQ(readiness_name(ReadinessStatus::Ready), "ready");
    EXPECT_EQ(readiness_name(ReadinessStatus::MissingEnv), "missing_env");
    EXPECT_EQ(readiness_name(ReadinessStatus::MissingCommand),
              "missing_command");
    EXPECT_EQ(readiness_name(ReadinessStatus::Incompatible), "incompatible");
    EXPECT_EQ(readiness_name(ReadinessStatus::Unknown), "unknown");
}

TEST(SkillsDepthPrereq, NormaliseString) {
    auto v = normalise_prerequisite_values(json{"FOO"});
    ASSERT_EQ(v.size(), 1u);
    EXPECT_EQ(v[0], "FOO");
}

TEST(SkillsDepthPrereq, NormaliseEmptyString) {
    auto v = normalise_prerequisite_values(json{"   "});
    EXPECT_TRUE(v.empty());
}

TEST(SkillsDepthPrereq, NormaliseList) {
    auto v = normalise_prerequisite_values(json::array({"A", "B", "", "C"}));
    ASSERT_EQ(v.size(), 3u);
    EXPECT_EQ(v[0], "A");
    EXPECT_EQ(v[2], "C");
}

TEST(SkillsDepthPrereq, NormaliseNullAndMisc) {
    EXPECT_TRUE(normalise_prerequisite_values(json{}).empty());
    EXPECT_TRUE(normalise_prerequisite_values(json::object()).empty());
}

TEST(SkillsDepthPrereq, CollectEnvVarsAndCommands) {
    json fm = {{"prerequisites",
                {{"env_vars", json::array({"OPENAI_API_KEY"})},
                 {"commands", json::array({"jq", "curl"})}}}};
    auto out = collect_prerequisite_values(fm);
    ASSERT_EQ(out.env_vars.size(), 1u);
    EXPECT_EQ(out.env_vars[0], "OPENAI_API_KEY");
    ASSERT_EQ(out.commands.size(), 2u);
    EXPECT_EQ(out.commands[1], "curl");
}

TEST(SkillsDepthPrereq, CollectEmptyWhenMissing) {
    auto out = collect_prerequisite_values(json::object());
    EXPECT_TRUE(out.env_vars.empty());
    EXPECT_TRUE(out.commands.empty());
}

TEST(SkillsDepthSetup, EmptyWhenMissing) {
    auto m = normalise_setup_metadata(json::object());
    EXPECT_FALSE(m.help.has_value());
    EXPECT_TRUE(m.collect_secrets.empty());
}

TEST(SkillsDepthSetup, HelpTrimmed) {
    json fm = {{"setup", {{"help", "  read docs  "}}}};
    auto m = normalise_setup_metadata(fm);
    ASSERT_TRUE(m.help.has_value());
    EXPECT_EQ(*m.help, "read docs");
}

TEST(SkillsDepthSetup, CollectSecretsListNormalisation) {
    json fm = {{"setup",
                {{"collect_secrets",
                  json::array(
                      {{{"env_var", "KEY1"},
                        {"prompt", "Enter your key"},
                        {"secret", true}},
                       {{"env_var", "KEY2"}, {"url", "https://example"}}})}}}};
    auto m = normalise_setup_metadata(fm);
    ASSERT_EQ(m.collect_secrets.size(), 2u);
    EXPECT_EQ(m.collect_secrets[0].env_var, "KEY1");
    EXPECT_EQ(m.collect_secrets[0].prompt, "Enter your key");
    EXPECT_TRUE(m.collect_secrets[0].secret);
    EXPECT_EQ(m.collect_secrets[1].env_var, "KEY2");
    EXPECT_EQ(m.collect_secrets[1].prompt, "Enter value for KEY2");
    EXPECT_EQ(m.collect_secrets[1].provider_url, "https://example");
}

TEST(SkillsDepthSetup, CollectSecretsSingleDictPromoted) {
    json fm = {{"setup",
                {{"collect_secrets",
                  {{"env_var", "ONE"}, {"provider_url", "https://one"}}}}}};
    auto m = normalise_setup_metadata(fm);
    ASSERT_EQ(m.collect_secrets.size(), 1u);
    EXPECT_EQ(m.collect_secrets[0].env_var, "ONE");
    EXPECT_EQ(m.collect_secrets[0].provider_url, "https://one");
}

TEST(SkillsDepthSetup, SecretsWithoutEnvVarDropped) {
    json fm = {{"setup",
                {{"collect_secrets",
                  json::array({{{"env_var", ""}}, {{"env_var", "GOOD"}}})}}}};
    auto m = normalise_setup_metadata(fm);
    ASSERT_EQ(m.collect_secrets.size(), 1u);
    EXPECT_EQ(m.collect_secrets[0].env_var, "GOOD");
}

TEST(SkillsDepthEnvName, ValidAndInvalid) {
    EXPECT_TRUE(is_valid_env_var_name("FOO"));
    EXPECT_TRUE(is_valid_env_var_name("_X"));
    EXPECT_TRUE(is_valid_env_var_name("FOO_BAR_1"));
    EXPECT_FALSE(is_valid_env_var_name(""));
    EXPECT_FALSE(is_valid_env_var_name("1FOO"));
    EXPECT_FALSE(is_valid_env_var_name("FOO-BAR"));
    EXPECT_FALSE(is_valid_env_var_name("FOO BAR"));
}

TEST(SkillsDepthRequiredEnv, EmptyForEmptyFrontmatter) {
    auto out = get_required_environment_variables(json::object());
    EXPECT_TRUE(out.empty());
}

TEST(SkillsDepthRequiredEnv, StructuredStringList) {
    json fm = {{"required_environment_variables",
                json::array({"FOO_KEY", "BAR_KEY"})}};
    auto out = get_required_environment_variables(fm);
    ASSERT_EQ(out.size(), 2u);
    EXPECT_EQ(out[0].name, "FOO_KEY");
    EXPECT_EQ(out[0].prompt, "Enter value for FOO_KEY");
}

TEST(SkillsDepthRequiredEnv, StructuredDictsRespectPromptAndHelp) {
    json fm = {
        {"required_environment_variables",
         json::array({{{"name", "API_KEY"},
                       {"prompt", "Paste your key"},
                       {"help", "see docs"},
                       {"required_for", "summariser"}}})}};
    auto out = get_required_environment_variables(fm);
    ASSERT_EQ(out.size(), 1u);
    EXPECT_EQ(out[0].prompt, "Paste your key");
    EXPECT_EQ(out[0].help, "see docs");
    EXPECT_EQ(out[0].required_for, "summariser");
}

TEST(SkillsDepthRequiredEnv, MergesLegacyAndSetupAndStructured) {
    json fm = {
        {"required_environment_variables", json::array({"KEY1"})},
        {"setup",
         {{"collect_secrets",
           json::array({{{"env_var", "KEY2"}, {"provider_url", "u"}}})}}},
        {"prerequisites", {{"env_vars", json::array({"KEY3", "KEY1"})}}}};
    auto out = get_required_environment_variables(fm);
    ASSERT_EQ(out.size(), 3u);
    EXPECT_EQ(out[0].name, "KEY1");
    EXPECT_EQ(out[1].name, "KEY2");
    EXPECT_EQ(out[2].name, "KEY3");
}

TEST(SkillsDepthRequiredEnv, RejectsInvalidName) {
    json fm = {{"required_environment_variables", json::array({"1BAD"})}};
    auto out = get_required_environment_variables(fm);
    EXPECT_TRUE(out.empty());
}

TEST(SkillsDepthRequiredEnv, LegacyOverride) {
    json fm = json::object();
    auto out = get_required_environment_variables(
        fm, std::optional<std::vector<std::string>>{{"OVERRIDE"}});
    ASSERT_EQ(out.size(), 1u);
    EXPECT_EQ(out[0].name, "OVERRIDE");
}

TEST(SkillsDepthTokens, EstimateDivides) {
    EXPECT_EQ(estimate_tokens(""), 0u);
    EXPECT_EQ(estimate_tokens("abcd"), 1u);
    EXPECT_EQ(estimate_tokens("abcdef"), 1u);
    EXPECT_EQ(estimate_tokens("abcdefgh"), 2u);
}

TEST(SkillsDepthTags, NullAndEmpty) {
    EXPECT_TRUE(parse_tags(json{}).empty());
    EXPECT_TRUE(parse_tags(json{""}).empty());
}

TEST(SkillsDepthTags, AlreadyList) {
    auto v = parse_tags(json::array({"a", "b"}));
    ASSERT_EQ(v.size(), 2u);
    EXPECT_EQ(v[0], "a");
    EXPECT_EQ(v[1], "b");
}

TEST(SkillsDepthTags, Bracketed) {
    auto v = parse_tags(json(std::string{"[x, y, z]"}));
    ASSERT_EQ(v.size(), 3u);
    EXPECT_EQ(v[2], "z");
}

TEST(SkillsDepthTags, CommaSeparated) {
    auto v = parse_tags(json(std::string{"x, y,z"}));
    ASSERT_EQ(v.size(), 3u);
}

TEST(SkillsDepthTags, StripsQuotes) {
    auto v = parse_tags(json(std::string{"\"a\", 'b'"}));
    ASSERT_EQ(v.size(), 2u);
    EXPECT_EQ(v[0], "a");
    EXPECT_EQ(v[1], "b");
}

TEST(SkillsDepthCategory, EnoughSegments) {
    EXPECT_EQ(category_from_relative_path("mlops/axolotl/SKILL.md"), "mlops");
    EXPECT_EQ(category_from_relative_path("/mlops/axolotl/SKILL.md"), "mlops");
}

TEST(SkillsDepthCategory, NotEnoughSegments) {
    EXPECT_EQ(category_from_relative_path("axolotl/SKILL.md"), "");
    EXPECT_EQ(category_from_relative_path("SKILL.md"), "");
    EXPECT_EQ(category_from_relative_path(""), "");
}

TEST(SkillsDepthPlatform, NormaliseAndSurface) {
    EXPECT_EQ(normalise_platform_id(" Telegram "), "telegram");
    EXPECT_TRUE(is_gateway_surface("Telegram"));
    EXPECT_TRUE(is_gateway_surface("signal"));
    EXPECT_FALSE(is_gateway_surface("cli"));
    EXPECT_FALSE(is_gateway_surface(""));
}

TEST(SkillsDepthDisabled, PrimaryAndPlatformLists) {
    EXPECT_TRUE(is_skill_disabled("foo", {"foo"}, {}));
    EXPECT_TRUE(is_skill_disabled("bar", {}, {"bar"}));
    EXPECT_TRUE(is_skill_disabled("Case", {"case"}, {}));
    EXPECT_FALSE(is_skill_disabled("baz", {"foo"}, {"bar"}));
    EXPECT_FALSE(is_skill_disabled("x", {}, {}));
}

TEST(SkillsDepthSetupHint, EmptyOnNoMissing) {
    EXPECT_EQ(format_setup_hint({}, false), "");
    EXPECT_EQ(format_setup_hint({}, true), "");
}

TEST(SkillsDepthSetupHint, LocalVsGateway) {
    auto local = format_setup_hint({"A", "B"}, false);
    EXPECT_NE(local.find("~/.hermes/.env"), std::string::npos);
    EXPECT_NE(local.find("A"), std::string::npos);
    auto gw = format_setup_hint({"A"}, true);
    EXPECT_NE(gw.find("/setup"), std::string::npos);
}

namespace {

bool env_not_persisted(std::string_view) { return false; }
bool env_all_persisted(std::string_view) { return true; }
bool env_mixed_persisted(std::string_view k) { return k == "A" || k == "C"; }

}  // namespace

TEST(SkillsDepthRemaining, AllMissing) {
    std::vector<RequiredEnvEntry> req{
        RequiredEnvEntry{"A", "p", "", ""},
        RequiredEnvEntry{"B", "p", "", ""},
    };
    auto missing = remaining_required_env_names(req, env_not_persisted);
    ASSERT_EQ(missing.size(), 2u);
    EXPECT_EQ(missing[0], "A");
}

TEST(SkillsDepthRemaining, NoneMissing) {
    std::vector<RequiredEnvEntry> req{
        RequiredEnvEntry{"A", "p", "", ""},
    };
    EXPECT_TRUE(
        remaining_required_env_names(req, env_all_persisted).empty());
}

TEST(SkillsDepthRemaining, Mixed) {
    std::vector<RequiredEnvEntry> req{
        RequiredEnvEntry{"A", "p", "", ""},
        RequiredEnvEntry{"B", "p", "", ""},
        RequiredEnvEntry{"C", "p", "", ""},
        RequiredEnvEntry{"D", "p", "", ""},
    };
    auto missing = remaining_required_env_names(req, env_mixed_persisted);
    ASSERT_EQ(missing.size(), 2u);
    EXPECT_EQ(missing[0], "B");
    EXPECT_EQ(missing[1], "D");
}

TEST(SkillsDepthRemaining, NullCallbackLeavesAllMissing) {
    std::vector<RequiredEnvEntry> req{
        RequiredEnvEntry{"A", "p", "", ""}};
    auto missing = remaining_required_env_names(req, nullptr);
    ASSERT_EQ(missing.size(), 1u);
}

TEST(SkillsDepthListing, RenderCount) {
    std::vector<SkillBriefEntry> es{
        SkillBriefEntry{"foo", "d", "mlops", {"a", "b"}, 42u},
        SkillBriefEntry{"bar", "d2", "web", {}, 10u},
    };
    auto out = render_skills_list(es);
    EXPECT_EQ(out["count"].get<std::size_t>(), 2u);
    EXPECT_EQ(out["skills"][0]["name"].get<std::string>(), "foo");
    EXPECT_EQ(out["skills"][0]["tokens"].get<std::size_t>(), 42u);
    EXPECT_EQ(out["skills"][1]["category"].get<std::string>(), "web");
}

TEST(SkillsDepthListing, FilterByCategory) {
    std::vector<SkillBriefEntry> es{
        SkillBriefEntry{"a", "", "mlops", {}, 0u},
        SkillBriefEntry{"b", "", "web", {}, 0u},
        SkillBriefEntry{"c", "", "MLOPS", {}, 0u},
    };
    auto out = filter_by_category(es, "mlops");
    EXPECT_EQ(out.size(), 2u);
    EXPECT_EQ(filter_by_category(es, "").size(), 3u);
    EXPECT_EQ(filter_by_category(es, "other").size(), 0u);
}

TEST(SkillsDepthListing, GroupByCategoryBuckets) {
    std::vector<SkillBriefEntry> es{
        SkillBriefEntry{"a", "d", "mlops", {}, 0u},
        SkillBriefEntry{"b", "d", "web", {}, 0u},
        SkillBriefEntry{"c", "d", "", {}, 0u},
        SkillBriefEntry{"d", "d", "mlops", {}, 0u},
    };
    auto out = group_by_category(es);
    EXPECT_EQ(out["total"].get<std::size_t>(), 4u);
    EXPECT_EQ(out["by_category"]["mlops"].size(), 2u);
    EXPECT_EQ(out["by_category"]["web"].size(), 1u);
    EXPECT_EQ(out["by_category"]["misc"].size(), 1u);
    ASSERT_EQ(out["categories"].size(), 3u);
    EXPECT_EQ(out["categories"][0].get<std::string>(), "mlops");
}

TEST(SkillsDepthFrontmatter, MissingOpener) {
    auto s = split_frontmatter("no frontmatter here");
    EXPECT_FALSE(s.ok);
}

TEST(SkillsDepthFrontmatter, MissingCloser) {
    auto s = split_frontmatter("---\nname: foo\n");
    EXPECT_FALSE(s.ok);
}

TEST(SkillsDepthFrontmatter, WellFormed) {
    auto s = split_frontmatter("---\nname: foo\ndescription: d\n---\nhello\n");
    ASSERT_TRUE(s.ok);
    EXPECT_NE(s.yaml_block.find("name: foo"), std::string::npos);
    EXPECT_EQ(s.body, "hello\n");
}

TEST(SkillsDepthFrontmatter, HandlesBOM) {
    auto s = split_frontmatter("\xEF\xBB\xBF---\nname: foo\n---\nbody\n");
    ASSERT_TRUE(s.ok);
    EXPECT_EQ(s.body, "body\n");
}

TEST(SkillsDepthFrontmatter, CRLFLineEndings) {
    auto s = split_frontmatter("---\r\nname: foo\r\n---\r\nbody\r\n");
    ASSERT_TRUE(s.ok);
    EXPECT_NE(s.yaml_block.find("name: foo"), std::string::npos);
}
