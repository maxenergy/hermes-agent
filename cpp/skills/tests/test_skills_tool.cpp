// Unit tests for hermes::skills::skills_tool — depth port of tools/skills_tool.py.

#include "hermes/skills/skills_tool.hpp"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <cstdlib>
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;
using nlohmann::json;
using namespace hermes::skills;

namespace {

fs::path make_temp_dir() {
    auto base = fs::temp_directory_path() /
                ("hermes_skills_tool_" +
                 std::to_string(static_cast<long long>(
                     std::chrono::steady_clock::now().time_since_epoch().count())));
    fs::create_directories(base);
    return base;
}

void write_file(const fs::path& p, std::string_view body) {
    fs::create_directories(p.parent_path());
    std::ofstream ofs(p);
    ofs << body;
}

}  // namespace

// ---------------------------------------------------------------------------
// Tags + name validation + platform normalization.
// ---------------------------------------------------------------------------

TEST(SkillsToolTags, ArrayOfStrings) {
    json j = json::array({"alpha", "beta", ""});
    auto tags = parse_tags(j);
    ASSERT_EQ(tags.size(), 2u);
    EXPECT_EQ(tags[0], "alpha");
    EXPECT_EQ(tags[1], "beta");
}

TEST(SkillsToolTags, BracketString) {
    json j = std::string("[a, 'b', \"c\"]");
    auto tags = parse_tags(j);
    ASSERT_EQ(tags.size(), 3u);
    EXPECT_EQ(tags[0], "a");
    EXPECT_EQ(tags[1], "b");
    EXPECT_EQ(tags[2], "c");
}

TEST(SkillsToolTags, CommaSeparated) {
    json j = std::string("x,y , z");
    auto tags = parse_tags(j);
    ASSERT_EQ(tags.size(), 3u);
    EXPECT_EQ(tags[2], "z");
}

TEST(SkillsToolTags, NullAndEmpty) {
    EXPECT_TRUE(parse_tags(json()).empty());
    EXPECT_TRUE(parse_tags(json("")).empty());
}

TEST(SkillsToolEnvName, Valid) {
    EXPECT_TRUE(is_valid_env_var_name("API_KEY"));
    EXPECT_TRUE(is_valid_env_var_name("_x"));
    EXPECT_TRUE(is_valid_env_var_name("A1"));
}

TEST(SkillsToolEnvName, Invalid) {
    EXPECT_FALSE(is_valid_env_var_name(""));
    EXPECT_FALSE(is_valid_env_var_name("1BAD"));
    EXPECT_FALSE(is_valid_env_var_name("A-B"));
    EXPECT_FALSE(is_valid_env_var_name("x y"));
}

TEST(SkillsToolPlatform, NormalizeTokens) {
    EXPECT_EQ(normalize_platform_token("macos"), "darwin");
    EXPECT_EQ(normalize_platform_token("OSX"), "darwin");
    EXPECT_EQ(normalize_platform_token("Windows"), "win32");
    EXPECT_EQ(normalize_platform_token("linux"), "linux");
    EXPECT_EQ(normalize_platform_token("freebsd"), "freebsd");
}

TEST(SkillsToolPlatform, MissingListMatchesAll) {
    EXPECT_TRUE(skill_matches_platform(json::object(), "linux"));
    EXPECT_TRUE(skill_matches_platform(json(), "linux"));
}

TEST(SkillsToolPlatform, ArrayMatch) {
    json j = {{"platforms", json::array({"linux", "macos"})}};
    EXPECT_TRUE(skill_matches_platform(j, "linux"));
    EXPECT_TRUE(skill_matches_platform(j, "linux-gnu"));
    EXPECT_TRUE(skill_matches_platform(j, "darwin"));
    EXPECT_FALSE(skill_matches_platform(j, "win32"));
}

TEST(SkillsToolPlatform, StringCommaList) {
    json j = {{"platforms", "linux, windows"}};
    EXPECT_TRUE(skill_matches_platform(j, "linux"));
    EXPECT_TRUE(skill_matches_platform(j, "win32"));
    EXPECT_FALSE(skill_matches_platform(j, "darwin"));
}

// ---------------------------------------------------------------------------
// Description + token estimate helpers.
// ---------------------------------------------------------------------------

TEST(SkillsToolDescription, TruncateLongDescription) {
    std::string too_long(kMaxSkillDescriptionLength + 50, 'x');
    auto out = truncate_description(too_long);
    EXPECT_EQ(out.size(), kMaxSkillDescriptionLength);
    EXPECT_EQ(out.substr(out.size() - 3), "...");
}

TEST(SkillsToolDescription, ShortPassesThrough) {
    EXPECT_EQ(truncate_description("hi"), "hi");
}

TEST(SkillsToolTokens, EstimateFourCharsPerToken) {
    EXPECT_EQ(estimate_tokens(""), 0u);
    EXPECT_EQ(estimate_tokens("12345678"), 2u);
}

// ---------------------------------------------------------------------------
// Prerequisites + setup metadata.
// ---------------------------------------------------------------------------

TEST(SkillsToolPrereq, CollectSimple) {
    json j = {
        {"prerequisites",
         {{"env_vars", json::array({"A", "B", ""})},
          {"commands", json::array({"curl"})}}},
    };
    auto p = collect_prerequisites(j);
    ASSERT_EQ(p.env_vars.size(), 2u);
    EXPECT_EQ(p.env_vars[0], "A");
    ASSERT_EQ(p.commands.size(), 1u);
    EXPECT_EQ(p.commands[0], "curl");
}

TEST(SkillsToolPrereq, MissingSection) {
    auto p = collect_prerequisites(json::object());
    EXPECT_TRUE(p.env_vars.empty());
    EXPECT_TRUE(p.commands.empty());
}

TEST(SkillsToolSetup, HelpAndCollectSecretsDictCoerced) {
    json j = {
        {"setup",
         {{"help", "  read docs  "},
          {"collect_secrets",
           {{"env_var", "TOKEN"}, {"url", "https://x"}}}}}};
    auto s = normalize_setup_metadata(j);
    ASSERT_TRUE(s.help.has_value());
    EXPECT_EQ(*s.help, "read docs");
    ASSERT_EQ(s.collect_secrets.size(), 1u);
    EXPECT_EQ(s.collect_secrets[0].env_var, "TOKEN");
    EXPECT_EQ(s.collect_secrets[0].provider_url, "https://x");
    EXPECT_EQ(s.collect_secrets[0].prompt, "Enter value for TOKEN");
}

TEST(SkillsToolSetup, SkipsMissingEnvVar) {
    json j = {
        {"setup",
         {{"collect_secrets", json::array({
                                   json::object({{"prompt", "x"}}),
                                   json::object({{"env_var", "KEY"},
                                                 {"prompt", "Enter KEY"}}),
                               })}}}};
    auto s = normalize_setup_metadata(j);
    ASSERT_EQ(s.collect_secrets.size(), 1u);
    EXPECT_EQ(s.collect_secrets[0].env_var, "KEY");
    EXPECT_EQ(s.collect_secrets[0].prompt, "Enter KEY");
}

TEST(SkillsToolRequired, MergesSourcesAndDedupes) {
    json j = {
        {"required_environment_variables",
         json::array({"OPENAI_API_KEY",
                      json::object({{"name", "ANOTHER"}, {"prompt", "go"}})})},
        {"setup",
         {{"help", "help-blob"},
          {"collect_secrets",
           json::array({json::object({{"env_var", "OPENAI_API_KEY"}}),
                        json::object({{"env_var", "SECRET2"},
                                      {"provider_url", "https://x"}})})}}},
        {"prerequisites", {{"env_vars", json::array({"LEGACY"})}}}};
    auto rv = get_required_environment_variables(j);
    ASSERT_EQ(rv.size(), 4u);
    EXPECT_EQ(rv[0].name, "OPENAI_API_KEY");
    // String entries get no inherited help.
    EXPECT_EQ(rv[0].help, "");
    EXPECT_EQ(rv[1].name, "ANOTHER");
    EXPECT_EQ(rv[1].prompt, "go");
    EXPECT_EQ(rv[2].name, "SECRET2");
    EXPECT_EQ(rv[2].help, "https://x");
    EXPECT_EQ(rv[3].name, "LEGACY");
}

TEST(SkillsToolRequired, DropsInvalidNames) {
    json j = {
        {"required_environment_variables",
         json::array({"1BAD", json::object({{"name", "GOOD"}})})}};
    auto rv = get_required_environment_variables(j);
    ASSERT_EQ(rv.size(), 1u);
    EXPECT_EQ(rv[0].name, "GOOD");
}

TEST(SkillsToolReadiness, GatewayHintNonEmpty) {
    EXPECT_FALSE(gateway_setup_hint().empty());
}

TEST(SkillsToolReadiness, SetupNoteEmptyWhenAvailable) {
    EXPECT_FALSE(
        build_setup_note(SkillReadinessStatus::Available, {}, {}).has_value());
}

TEST(SkillsToolReadiness, SetupNoteWithMissingAndHelp) {
    auto note = build_setup_note(SkillReadinessStatus::SetupNeeded,
                                 {"A", "B"}, "visit the docs");
    ASSERT_TRUE(note.has_value());
    EXPECT_NE(note->find("A, B"), std::string::npos);
    EXPECT_NE(note->find("visit the docs"), std::string::npos);
}

TEST(SkillsToolReadiness, SetupNoteDefaultText) {
    auto note = build_setup_note(SkillReadinessStatus::SetupNeeded, {}, {});
    ASSERT_TRUE(note.has_value());
    EXPECT_NE(note->find("required prerequisites"), std::string::npos);
}

// ---------------------------------------------------------------------------
// dot-env parsing + readiness snapshot.
// ---------------------------------------------------------------------------

TEST(SkillsToolDotenv, ParsesSimpleAndQuoted) {
    std::string env = R"(# comment line

FOO=bar
BAZ="quoted value"
EMPTY=
QX='single'
)";
    auto m = parse_dotenv(env);
    EXPECT_EQ(m["FOO"], "bar");
    EXPECT_EQ(m["BAZ"], "quoted value");
    EXPECT_EQ(m["QX"], "single");
    EXPECT_EQ(m["EMPTY"], "");
}

TEST(SkillsToolDotenv, IgnoresBlankAndMalformed) {
    auto m = parse_dotenv("\n  \n NOT_AN_ASSIGNMENT \n =val\n A=1\n");
    EXPECT_EQ(m.size(), 1u);
    EXPECT_EQ(m["A"], "1");
}

TEST(SkillsToolEnv, PersistedFromSnapshot) {
    std::unordered_map<std::string, std::string> snap{{"X", "yes"},
                                                       {"EMPTY", ""}};
    EXPECT_TRUE(is_env_var_persisted("X", snap));
    EXPECT_FALSE(is_env_var_persisted("EMPTY", snap));
}

TEST(SkillsToolEnv, PersistedFallsBackToProcessEnv) {
    std::unordered_map<std::string, std::string> snap;
    ::setenv("HERMES_TEST_ENV_DIRECT", "value", 1);
    EXPECT_TRUE(is_env_var_persisted("HERMES_TEST_ENV_DIRECT", snap));
    ::unsetenv("HERMES_TEST_ENV_DIRECT");
    EXPECT_FALSE(is_env_var_persisted("HERMES_TEST_ENV_DIRECT", snap));
}

TEST(SkillsToolReadiness, AvailableWithNoRequirements) {
    auto r = classify_readiness(json::object(), {});
    EXPECT_EQ(r.status, SkillReadinessStatus::Available);
    EXPECT_TRUE(r.missing_env_vars.empty());
    EXPECT_FALSE(r.note.has_value());
}

TEST(SkillsToolReadiness, SetupNeededWhenRequiredMissing) {
    json fm = {
        {"required_environment_variables", json::array({"NEEDED_KEY"})},
        {"setup", {{"help", "install the gizmo"}}}};
    auto r = classify_readiness(fm, {});
    EXPECT_EQ(r.status, SkillReadinessStatus::SetupNeeded);
    ASSERT_EQ(r.missing_env_vars.size(), 1u);
    EXPECT_EQ(r.missing_env_vars[0], "NEEDED_KEY");
    ASSERT_TRUE(r.note.has_value());
    EXPECT_NE(r.note->find("install the gizmo"), std::string::npos);
}

TEST(SkillsToolReadiness, AvailableOnceSnapshotHasValue) {
    json fm = {
        {"required_environment_variables", json::array({"KEY"})}};
    std::unordered_map<std::string, std::string> snap{{"KEY", "v"}};
    auto r = classify_readiness(fm, snap);
    EXPECT_EQ(r.status, SkillReadinessStatus::Available);
}

TEST(SkillsToolReadiness, RemainingMergesCaptureMissing) {
    std::vector<RequiredEnvVar> req{
        {"A", "a?", "", ""}, {"B", "b?", "", ""}, {"C", "c?", "", ""}};
    std::unordered_map<std::string, std::string> snap{{"B", "val"}};
    auto rem =
        remaining_required_environment_names(req, {"A"}, snap);
    ASSERT_EQ(rem.size(), 2u);
    EXPECT_EQ(rem[0], "A");
    EXPECT_EQ(rem[1], "C");
}

// ---------------------------------------------------------------------------
// Discovery: category, DESCRIPTION.md, scan, merge.
// ---------------------------------------------------------------------------

TEST(SkillsToolDiscovery, CategoryFromPath) {
    auto root = make_temp_dir();
    auto skill_md = root / "mlops" / "axolotl" / "SKILL.md";
    fs::create_directories(skill_md.parent_path());
    write_file(skill_md, "---\nname: axolotl\n---\n");
    std::vector<fs::path> roots{root};
    EXPECT_EQ(category_from_path(skill_md, roots), "mlops");
    fs::remove_all(root);
}

TEST(SkillsToolDiscovery, CategoryEmptyForFlatLayout) {
    auto root = make_temp_dir();
    auto skill_md = root / "axolotl" / "SKILL.md";
    fs::create_directories(skill_md.parent_path());
    write_file(skill_md, "---\n---\n");
    EXPECT_EQ(category_from_path(skill_md, {root}), std::string{});
    fs::remove_all(root);
}

TEST(SkillsToolDiscovery, LoadCategoryDescription) {
    auto root = make_temp_dir();
    auto desc = root / "mlops" / "DESCRIPTION.md";
    fs::create_directories(desc.parent_path());
    write_file(desc, "---\ndescription: MLOps skills live here\n---\n# body\n");
    auto d = load_category_description(root / "mlops");
    ASSERT_TRUE(d.has_value());
    EXPECT_EQ(*d, "MLOps skills live here");
    fs::remove_all(root);
}

TEST(SkillsToolDiscovery, LoadCategoryDescriptionFromBody) {
    auto root = make_temp_dir();
    auto desc = root / "cat" / "DESCRIPTION.md";
    fs::create_directories(desc.parent_path());
    write_file(desc, "# Cat\nFirst real line goes here.\nSecond.\n");
    auto d = load_category_description(root / "cat");
    ASSERT_TRUE(d.has_value());
    EXPECT_EQ(*d, "First real line goes here.");
    fs::remove_all(root);
}

TEST(SkillsToolDiscovery, LoadCategoryDescriptionMissing) {
    auto root = make_temp_dir();
    fs::create_directories(root / "empty");
    EXPECT_FALSE(load_category_description(root / "empty").has_value());
    fs::remove_all(root);
}

TEST(SkillsToolDiscovery, FindSkillsInDir) {
    auto root = make_temp_dir();
    write_file(root / "alpha" / "SKILL.md",
               "---\nname: alpha\ndescription: A skill\n---\nbody\n");
    write_file(root / "cat" / "beta" / "SKILL.md",
               "---\nname: beta\n---\n# Title\nFirst paragraph.\n");
    // Platform-gated skill — should be skipped when not on darwin.
    write_file(root / "gamma" / "SKILL.md",
               "---\nname: gamma\nplatforms: macos\n---\n");
    auto found = find_skills_in_dir(root, "linux");
    // Expect at least alpha + beta found (order not guaranteed).
    std::unordered_map<std::string, SkillListEntry> by_name;
    for (auto& e : found) by_name[e.name] = e;
    ASSERT_TRUE(by_name.count("alpha"));
    EXPECT_EQ(by_name["alpha"].description, "A skill");
    ASSERT_TRUE(by_name.count("beta"));
    // Falls back to first non-header body line.
    EXPECT_EQ(by_name["beta"].description, "First paragraph.");
    EXPECT_FALSE(by_name.count("gamma"));
    fs::remove_all(root);
}

TEST(SkillsToolDiscovery, MergeKeepsFirstAndDropsDisabled) {
    SkillListEntry a{"alpha", "d1", "", "/x"};
    SkillListEntry b{"beta", "d2", "", "/y"};
    SkillListEntry a_dup{"alpha", "other", "", "/z"};
    auto merged = merge_skill_lists({{a, b}, {a_dup}}, {"beta"});
    ASSERT_EQ(merged.size(), 1u);
    EXPECT_EQ(merged[0].name, "alpha");
    EXPECT_EQ(merged[0].description, "d1");
}

TEST(SkillsToolView, BuildEnvelope) {
    std::string md =
        "---\nname: foo\ndescription: Demo skill\n---\n# heading\nbody text\n";
    auto env = build_skill_view_envelope("foo", md);
    EXPECT_EQ(env["name"], "foo");
    EXPECT_EQ(env["description"], "Demo skill");
    EXPECT_NE(env["body"].get<std::string>().find("body text"), std::string::npos);
    EXPECT_TRUE(env.contains("tokens"));
    EXPECT_TRUE(env.contains("frontmatter"));
}

TEST(SkillsToolView, EnvelopeWithoutFrontmatter) {
    auto env = build_skill_view_envelope("x", "just body text\n");
    EXPECT_EQ(env["description"], "");
    EXPECT_EQ(env["body"], "just body text\n");
}

TEST(SkillsToolStatus, ToString) {
    EXPECT_EQ(to_string(SkillReadinessStatus::Available), "available");
    EXPECT_EQ(to_string(SkillReadinessStatus::SetupNeeded), "setup_needed");
    EXPECT_EQ(to_string(SkillReadinessStatus::Unsupported), "unsupported");
}
