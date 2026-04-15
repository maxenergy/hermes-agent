// Tests for hermes/tools/skill_manager_tool_depth.hpp.
#include "hermes/tools/skill_manager_tool_depth.hpp"

#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

using namespace hermes::tools::skill_manager::depth;
using json = nlohmann::json;

TEST(SkillManagerDepthAction, Parse) {
    EXPECT_EQ(parse_action("list"), Action::List);
    EXPECT_EQ(parse_action("CREATE"), Action::Create);
    EXPECT_EQ(parse_action(" patch "), Action::Patch);
    EXPECT_EQ(parse_action("write_file"), Action::WriteFile);
    EXPECT_EQ(parse_action("remove_file"), Action::RemoveFile);
    EXPECT_EQ(parse_action(""), Action::Unknown);
    EXPECT_EQ(parse_action("bogus"), Action::Unknown);
}

TEST(SkillManagerDepthAction, NameRoundTrip) {
    EXPECT_EQ(action_name(Action::Install), "install");
    EXPECT_EQ(action_name(Action::View), "view");
    EXPECT_EQ(action_name(Action::Unknown), "unknown");
}

TEST(SkillManagerDepthAction, RequiresName) {
    EXPECT_TRUE(action_requires_name(Action::Create));
    EXPECT_TRUE(action_requires_name(Action::Delete));
    EXPECT_TRUE(action_requires_name(Action::View));
    EXPECT_FALSE(action_requires_name(Action::List));
    EXPECT_FALSE(action_requires_name(Action::Search));
}

TEST(SkillManagerDepthAction, RequiresContent) {
    EXPECT_TRUE(action_requires_content(Action::Create));
    EXPECT_TRUE(action_requires_content(Action::Edit));
    EXPECT_FALSE(action_requires_content(Action::Patch));
    EXPECT_FALSE(action_requires_content(Action::Delete));
}

TEST(SkillManagerDepthSize, WithinLimit) {
    EXPECT_EQ(validate_content_size(1000, "SKILL.md"), "");
    EXPECT_EQ(validate_content_size(kMaxSkillContentChars, "SKILL.md"), "");
}

TEST(SkillManagerDepthSize, OverLimit) {
    auto err = validate_content_size(kMaxSkillContentChars + 1u, "SKILL.md");
    EXPECT_NE(err.find("100,001"), std::string::npos);
    EXPECT_NE(err.find("limit"), std::string::npos);
}

TEST(SkillManagerDepthSize, FileBytesWithinLimit) {
    EXPECT_EQ(validate_file_bytes(1024, "f.bin"), "");
    EXPECT_EQ(validate_file_bytes(kMaxSkillFileBytes, "f.bin"), "");
}

TEST(SkillManagerDepthSize, FileBytesOverLimit) {
    auto err = validate_file_bytes(kMaxSkillFileBytes + 1u, "f.bin");
    EXPECT_NE(err.find("f.bin"), std::string::npos);
    EXPECT_NE(err.find("1 MiB"), std::string::npos);
}

TEST(SkillManagerDepthFrontmatter, Empty) {
    auto r = check_frontmatter_structure("");
    EXPECT_FALSE(r.ok);
    EXPECT_NE(r.error.find("empty"), std::string::npos);
}

TEST(SkillManagerDepthFrontmatter, Whitespace) {
    auto r = check_frontmatter_structure("   \n\t  ");
    EXPECT_FALSE(r.ok);
    EXPECT_NE(r.error.find("empty"), std::string::npos);
}

TEST(SkillManagerDepthFrontmatter, MissingOpener) {
    auto r = check_frontmatter_structure("name: foo\n");
    EXPECT_FALSE(r.ok);
    EXPECT_NE(r.error.find("start with"), std::string::npos);
}

TEST(SkillManagerDepthFrontmatter, MissingCloser) {
    auto r = check_frontmatter_structure("---\nname: foo\n");
    EXPECT_FALSE(r.ok);
    EXPECT_NE(r.error.find("not closed"), std::string::npos);
}

TEST(SkillManagerDepthFrontmatter, MissingBody) {
    auto r = check_frontmatter_structure("---\nname: foo\n---\n");
    EXPECT_FALSE(r.ok);
    EXPECT_NE(r.error.find("content after"), std::string::npos);
}

TEST(SkillManagerDepthFrontmatter, WellFormed) {
    auto r = check_frontmatter_structure(
        "---\nname: foo\ndescription: d\n---\nbody here\n");
    EXPECT_TRUE(r.ok);
    EXPECT_NE(r.yaml_block.find("name: foo"), std::string::npos);
    EXPECT_EQ(r.body, "body here\n");
}

TEST(SkillManagerDepthFrontmatterKeys, NotMapping) {
    EXPECT_NE(validate_frontmatter_keys(json::array({1, 2})), "");
    EXPECT_NE(validate_frontmatter_keys(json{"x"}), "");
}

TEST(SkillManagerDepthFrontmatterKeys, MissingName) {
    EXPECT_NE(validate_frontmatter_keys(json{{"description", "d"}}), "");
}

TEST(SkillManagerDepthFrontmatterKeys, MissingDescription) {
    EXPECT_NE(validate_frontmatter_keys(json{{"name", "n"}}), "");
}

TEST(SkillManagerDepthFrontmatterKeys, DescriptionTooLong) {
    std::string big(1500, 'x');
    auto err = validate_frontmatter_keys(
        json{{"name", "n"}, {"description", big}});
    EXPECT_NE(err.find("1024"), std::string::npos);
}

TEST(SkillManagerDepthFrontmatterKeys, Ok) {
    auto err = validate_frontmatter_keys(
        json{{"name", "n"}, {"description", "short"}});
    EXPECT_EQ(err, "");
}

TEST(SkillManagerDepthPath, Empty) {
    auto r = normalise_relative_path("");
    EXPECT_NE(r.error, "");
}

TEST(SkillManagerDepthPath, AbsoluteRejected) {
    EXPECT_NE(normalise_relative_path("/etc/passwd").error, "");
    EXPECT_NE(normalise_relative_path("C:/Windows").error, "");
}

TEST(SkillManagerDepthPath, TraversalRejected) {
    EXPECT_NE(normalise_relative_path("references/../etc").error, "");
    EXPECT_NE(normalise_relative_path("../foo").error, "");
}

TEST(SkillManagerDepthPath, WrongTopSegmentRejected) {
    auto r = normalise_relative_path("src/foo.md");
    EXPECT_NE(r.error.find("File must be under"), std::string::npos);
}

TEST(SkillManagerDepthPath, MissingFilenameRejected) {
    auto r = normalise_relative_path("references");
    EXPECT_NE(r.error.find("Provide a file path"), std::string::npos);
}

TEST(SkillManagerDepthPath, CollapsesDotSegments) {
    auto r = normalise_relative_path("references/./sub/file.md");
    EXPECT_EQ(r.error, "");
    EXPECT_EQ(r.cleaned, "references/sub/file.md");
}

TEST(SkillManagerDepthPath, AllowedSubdirs) {
    for (const auto* top :
         {"references", "templates", "scripts", "assets"}) {
        std::string p{std::string{top} + "/a.md"};
        auto r = normalise_relative_path(p);
        EXPECT_EQ(r.error, "");
        EXPECT_TRUE(first_segment_is_allowed(r.cleaned));
    }
}

TEST(SkillManagerDepthPath, BackslashesNormalised) {
    auto r = normalise_relative_path("references\\sub\\file.md");
    EXPECT_EQ(r.error, "");
    EXPECT_EQ(r.cleaned, "references/sub/file.md");
}

TEST(SkillManagerDepthPath, FirstSegmentAllowedRejectsUnknown) {
    EXPECT_FALSE(first_segment_is_allowed("src/x"));
    EXPECT_FALSE(first_segment_is_allowed(""));
}

TEST(SkillManagerDepthAtomic, TempName) {
    EXPECT_EQ(atomic_temp_name("SKILL.md", 0xdeadu), ".SKILL.md.tmp.dead");
    EXPECT_EQ(atomic_temp_name("foo", 0u), ".foo.tmp.0");
}

TEST(SkillManagerDepthPayload, Error) {
    auto j = error_payload("broken");
    EXPECT_FALSE(j["success"].get<bool>());
    EXPECT_EQ(j["error"].get<std::string>(), "broken");
}

TEST(SkillManagerDepthPayload, SuccessMessage) {
    auto j = success_message_payload("done");
    EXPECT_TRUE(j["success"].get<bool>());
    EXPECT_EQ(j["message"].get<std::string>(), "done");
}

TEST(SkillManagerDepthPayload, NotFound) {
    auto j = not_found_payload("my-skill");
    EXPECT_FALSE(j["success"].get<bool>());
    EXPECT_NE(j["error"].get<std::string>().find("my-skill"),
              std::string::npos);
}

TEST(SkillManagerDepthPayload, StructuralBreak) {
    auto j = structural_break_payload("frontmatter not closed");
    std::string msg = j["error"].get<std::string>();
    EXPECT_NE(msg.find("Patch would break"), std::string::npos);
    EXPECT_NE(msg.find("frontmatter not closed"), std::string::npos);
}

TEST(SkillManagerDepthSearch, EmptyQueryReturnsAll) {
    std::vector<MinimalSkill> in{
        MinimalSkill{"foo", "desc"},
        MinimalSkill{"bar", "other"},
    };
    auto out = substring_search(in, "");
    EXPECT_EQ(out.size(), 2u);
}

TEST(SkillManagerDepthSearch, MatchName) {
    std::vector<MinimalSkill> in{
        MinimalSkill{"foo-bar", "desc"},
        MinimalSkill{"baz", "other"},
    };
    auto out = substring_search(in, "FOO");
    ASSERT_EQ(out.size(), 1u);
    EXPECT_EQ(out[0].name, "foo-bar");
}

TEST(SkillManagerDepthSearch, MatchDescription) {
    std::vector<MinimalSkill> in{
        MinimalSkill{"foo", "contains MAGIC word"},
    };
    EXPECT_EQ(substring_search(in, "magic").size(), 1u);
}

TEST(SkillManagerDepthSearch, NoMatch) {
    std::vector<MinimalSkill> in{MinimalSkill{"foo", "d"}};
    EXPECT_EQ(substring_search(in, "nope").size(), 0u);
}

TEST(SkillManagerDepthPatch, EmptyOldString) {
    auto r = exact_replace("hello", "", "x", false);
    EXPECT_NE(r.error, "");
}

TEST(SkillManagerDepthPatch, NoMatch) {
    auto r = exact_replace("hello world", "foo", "bar", false);
    EXPECT_NE(r.error, "");
}

TEST(SkillManagerDepthPatch, UniqueReplace) {
    auto r = exact_replace("hello world", "world", "there", false);
    EXPECT_EQ(r.error, "");
    EXPECT_EQ(r.output, "hello there");
    EXPECT_EQ(r.replacements, 1u);
}

TEST(SkillManagerDepthPatch, AmbiguousWithoutReplaceAll) {
    auto r = exact_replace("a b a b", "a", "X", false);
    EXPECT_NE(r.error, "");
    EXPECT_NE(r.error.find("2 times"), std::string::npos);
}

TEST(SkillManagerDepthPatch, AmbiguousWithReplaceAll) {
    auto r = exact_replace("a b a b", "a", "X", true);
    EXPECT_EQ(r.error, "");
    EXPECT_EQ(r.output, "X b X b");
    EXPECT_EQ(r.replacements, 2u);
}

TEST(SkillManagerDepthPatch, ReplaceWithEmpty) {
    auto r = exact_replace("hello world", "hello ", "", false);
    EXPECT_EQ(r.error, "");
    EXPECT_EQ(r.output, "world");
    EXPECT_EQ(r.replacements, 1u);
}

TEST(SkillManagerDepthPatch, OverlappingSafe) {
    auto r = exact_replace("aaaa", "aa", "b", true);
    EXPECT_EQ(r.error, "");
    // Non-overlapping scan: "aa"+"aa" -> "bb".
    EXPECT_EQ(r.output, "bb");
    EXPECT_EQ(r.replacements, 2u);
}
