// Tests for profiles_helpers — pure-logic port of hermes_cli/profiles.py.
#include "hermes/cli/profiles_helpers.hpp"

#include <gtest/gtest.h>

namespace ph = hermes::cli::profiles_helpers;
namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// Reserved / validation.
// ---------------------------------------------------------------------------

TEST(ProfilesHelpersValid, AcceptsSimple) {
    EXPECT_TRUE(ph::is_valid_profile_id("coder"));
    EXPECT_TRUE(ph::is_valid_profile_id("my-profile"));
    EXPECT_TRUE(ph::is_valid_profile_id("my_profile"));
    EXPECT_TRUE(ph::is_valid_profile_id("a1b2"));
    EXPECT_TRUE(ph::is_valid_profile_id("0abc"));
}

TEST(ProfilesHelpersValid, RejectsUppercase) {
    EXPECT_FALSE(ph::is_valid_profile_id("Coder"));
    EXPECT_FALSE(ph::is_valid_profile_id("MY-profile"));
}

TEST(ProfilesHelpersValid, RejectsLeadingDashUnderscore) {
    EXPECT_FALSE(ph::is_valid_profile_id("-coder"));
    EXPECT_FALSE(ph::is_valid_profile_id("_coder"));
}

TEST(ProfilesHelpersValid, RejectsEmptyAndOverlong) {
    EXPECT_FALSE(ph::is_valid_profile_id(""));
    EXPECT_FALSE(ph::is_valid_profile_id(std::string(65, 'a')));
    EXPECT_TRUE(ph::is_valid_profile_id(std::string(64, 'a')));
}

TEST(ProfilesHelpersValid, RejectsSpecialChars) {
    EXPECT_FALSE(ph::is_valid_profile_id("a.b"));
    EXPECT_FALSE(ph::is_valid_profile_id("a/b"));
    EXPECT_FALSE(ph::is_valid_profile_id("a b"));
}

TEST(ProfilesHelpersValid, DefaultIsAccepted) {
    EXPECT_NO_THROW(ph::validate_profile_name("default"));
    EXPECT_NO_THROW(ph::validate_profile_name("coder"));
    EXPECT_THROW(ph::validate_profile_name("Bad!"), std::invalid_argument);
}

// ---------------------------------------------------------------------------
// Collision detection.
// ---------------------------------------------------------------------------

TEST(ProfilesHelpersCollision, ReservedNames) {
    EXPECT_TRUE(
        ph::check_alias_collision("root", {}, {}).has_value());
    EXPECT_TRUE(
        ph::check_alias_collision("sudo", {}, {}).has_value());
}

TEST(ProfilesHelpersCollision, HermesSubcommands) {
    EXPECT_TRUE(
        ph::check_alias_collision("chat", {}, {}).has_value());
    EXPECT_TRUE(
        ph::check_alias_collision("gateway", {}, {}).has_value());
}

TEST(ProfilesHelpersCollision, ExistingCommand) {
    const auto msg {ph::check_alias_collision(
        "mycoder", {"/usr/bin/mycoder"}, {})};
    ASSERT_TRUE(msg.has_value());
    EXPECT_NE(msg->find("mycoder"), std::string::npos);
}

TEST(ProfilesHelpersCollision, OwnWrapperAllowed) {
    const auto msg {ph::check_alias_collision(
        "mycoder",
        {"/home/u/.local/bin/mycoder"},
        {"/home/u/.local/bin/mycoder"})};
    EXPECT_FALSE(msg.has_value());
}

TEST(ProfilesHelpersCollision, Safe) {
    EXPECT_FALSE(ph::check_alias_collision("unique", {}, {}).has_value());
}

// ---------------------------------------------------------------------------
// Paths.
// ---------------------------------------------------------------------------

TEST(ProfilesHelpersPaths, DefaultMapsToRoot) {
    EXPECT_EQ(ph::get_profile_dir("default", "/home/u/.hermes"),
              fs::path {"/home/u/.hermes"});
}

TEST(ProfilesHelpersPaths, NamedProfileDir) {
    EXPECT_EQ(ph::get_profile_dir("coder", "/home/u/.hermes"),
              fs::path {"/home/u/.hermes/profiles/coder"});
}

TEST(ProfilesHelpersPaths, ProfilesRoot) {
    EXPECT_EQ(ph::get_profiles_root("/home/u/.hermes"),
              fs::path {"/home/u/.hermes/profiles"});
}

TEST(ProfilesHelpersPaths, ActiveProfile) {
    EXPECT_EQ(ph::get_active_profile_path("/home/u/.hermes"),
              fs::path {"/home/u/.hermes/active_profile"});
}

// ---------------------------------------------------------------------------
// Wrappers.
// ---------------------------------------------------------------------------

TEST(ProfilesHelpersWrapper, RenderScript) {
    const std::string s {ph::render_wrapper_script("coder")};
    EXPECT_NE(s.find("#!/bin/sh"), std::string::npos);
    EXPECT_NE(s.find("exec hermes -p coder \"$@\""), std::string::npos);
}

TEST(ProfilesHelpersWrapper, IsHermesWrapper) {
    EXPECT_TRUE(ph::is_hermes_wrapper("#!/bin/sh\nexec hermes -p coder"));
    EXPECT_FALSE(ph::is_hermes_wrapper("echo hi"));
}

// ---------------------------------------------------------------------------
// Archive path safety.
// ---------------------------------------------------------------------------

TEST(ProfilesHelpersArchive, SplitsBasic) {
    EXPECT_EQ(ph::normalize_archive_member("coder/config.yaml"),
              (std::vector<std::string> {"coder", "config.yaml"}));
}

TEST(ProfilesHelpersArchive, DropsDot) {
    EXPECT_EQ(ph::normalize_archive_member("./coder/x"),
              (std::vector<std::string> {"coder", "x"}));
}

TEST(ProfilesHelpersArchive, RejectsAbsolute) {
    EXPECT_THROW(ph::normalize_archive_member("/etc/passwd"),
                 std::invalid_argument);
}

TEST(ProfilesHelpersArchive, RejectsDriveLetter) {
    EXPECT_THROW(ph::normalize_archive_member("C:/x"),
                 std::invalid_argument);
}

TEST(ProfilesHelpersArchive, RejectsDotDot) {
    EXPECT_THROW(ph::normalize_archive_member("coder/../etc"),
                 std::invalid_argument);
}

TEST(ProfilesHelpersArchive, BackslashNormalised) {
    EXPECT_EQ(ph::normalize_archive_member("coder\\a"),
              (std::vector<std::string> {"coder", "a"}));
}

// ---------------------------------------------------------------------------
// gateway.pid parse.
// ---------------------------------------------------------------------------

TEST(ProfilesHelpersGatewayPid, BareInteger) {
    EXPECT_EQ(ph::parse_gateway_pid_file("12345\n"), std::optional<int> {12345});
}

TEST(ProfilesHelpersGatewayPid, JsonObject) {
    EXPECT_EQ(ph::parse_gateway_pid_file(R"({"pid": 777, "port": 8080})"),
              std::optional<int> {777});
}

TEST(ProfilesHelpersGatewayPid, Empty) {
    EXPECT_FALSE(ph::parse_gateway_pid_file("").has_value());
    EXPECT_FALSE(ph::parse_gateway_pid_file("   \n").has_value());
}

TEST(ProfilesHelpersGatewayPid, Garbage) {
    EXPECT_FALSE(ph::parse_gateway_pid_file("abc").has_value());
    EXPECT_FALSE(ph::parse_gateway_pid_file("{\"foo\": 1}").has_value());
}

// ---------------------------------------------------------------------------
// Completion scripts.
// ---------------------------------------------------------------------------

TEST(ProfilesHelpersCompletion, Bash) {
    const std::string s {ph::generate_bash_completion()};
    EXPECT_NE(s.find("_hermes_completion"), std::string::npos);
    EXPECT_NE(s.find("complete -F"), std::string::npos);
}

TEST(ProfilesHelpersCompletion, Zsh) {
    const std::string s {ph::generate_zsh_completion()};
    EXPECT_NE(s.find("#compdef hermes"), std::string::npos);
}

// ---------------------------------------------------------------------------
// Export exclusions.
// ---------------------------------------------------------------------------

TEST(ProfilesHelpersExport, ExcludesInfra) {
    EXPECT_TRUE(ph::is_default_export_excluded("hermes-agent"));
    EXPECT_TRUE(ph::is_default_export_excluded(".env"));
    EXPECT_TRUE(ph::is_default_export_excluded("auth.json"));
    EXPECT_TRUE(ph::is_default_export_excluded("profiles"));
}

TEST(ProfilesHelpersExport, IncludesNormalFiles) {
    EXPECT_FALSE(ph::is_default_export_excluded("config.yaml"));
    EXPECT_FALSE(ph::is_default_export_excluded("skills"));
    EXPECT_FALSE(ph::is_default_export_excluded("SOUL.md"));
}

// ---------------------------------------------------------------------------
// Rename validation.
// ---------------------------------------------------------------------------

TEST(ProfilesHelpersRename, OK) {
    EXPECT_NO_THROW(ph::validate_rename("a", "b"));
}

TEST(ProfilesHelpersRename, RejectsDefault) {
    EXPECT_THROW(ph::validate_rename("default", "x"), std::invalid_argument);
    EXPECT_THROW(ph::validate_rename("a", "default"), std::invalid_argument);
}

TEST(ProfilesHelpersRename, RejectsSame) {
    EXPECT_THROW(ph::validate_rename("a", "a"), std::invalid_argument);
}
