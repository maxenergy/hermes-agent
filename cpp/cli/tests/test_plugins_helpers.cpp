// Tests for plugins_helpers — pure-logic port of `hermes_cli/plugins_cmd.py`.
#include "hermes/cli/plugins_helpers.hpp"

#include <gtest/gtest.h>

#include <filesystem>

namespace ph = hermes::cli::plugins_helpers;
namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// sanitize_plugin_name
// ---------------------------------------------------------------------------

TEST(PluginsHelpersSanitize, AcceptsSimpleName) {
    const fs::path root {"/tmp/hermes/plugins"};
    const auto target {ph::sanitize_plugin_name("my-plugin", root)};
    EXPECT_EQ(target, root / "my-plugin");
}

TEST(PluginsHelpersSanitize, RejectsEmpty) {
    EXPECT_THROW(ph::sanitize_plugin_name("", "/tmp/plugins"),
                 std::invalid_argument);
}

TEST(PluginsHelpersSanitize, RejectsDot) {
    EXPECT_THROW(ph::sanitize_plugin_name(".", "/tmp/plugins"),
                 std::invalid_argument);
}

TEST(PluginsHelpersSanitize, RejectsDotDot) {
    EXPECT_THROW(ph::sanitize_plugin_name("..", "/tmp/plugins"),
                 std::invalid_argument);
}

TEST(PluginsHelpersSanitize, RejectsSlash) {
    EXPECT_THROW(ph::sanitize_plugin_name("a/b", "/tmp/plugins"),
                 std::invalid_argument);
}

TEST(PluginsHelpersSanitize, RejectsBackslash) {
    EXPECT_THROW(ph::sanitize_plugin_name("a\\b", "/tmp/plugins"),
                 std::invalid_argument);
}

TEST(PluginsHelpersSanitize, RejectsTraversal) {
    EXPECT_THROW(ph::sanitize_plugin_name("..bad", "/tmp/plugins"),
                 std::invalid_argument);
}

// ---------------------------------------------------------------------------
// classify_url / is_insecure_scheme
// ---------------------------------------------------------------------------

TEST(PluginsHelpersUrl, ClassifiesHttps) {
    EXPECT_EQ(ph::classify_url("https://github.com/a/b.git"),
              ph::UrlScheme::Https);
}

TEST(PluginsHelpersUrl, ClassifiesHttpAsInsecure) {
    EXPECT_EQ(ph::classify_url("http://example.com/x.git"),
              ph::UrlScheme::Http);
    EXPECT_TRUE(ph::is_insecure_scheme("http://x"));
}

TEST(PluginsHelpersUrl, ClassifiesSsh) {
    EXPECT_EQ(ph::classify_url("git@github.com:a/b.git"),
              ph::UrlScheme::Ssh);
    EXPECT_EQ(ph::classify_url("ssh://git@github.com/a/b.git"),
              ph::UrlScheme::Ssh);
    EXPECT_FALSE(ph::is_insecure_scheme("git@host:a/b"));
}

TEST(PluginsHelpersUrl, ClassifiesFileAsInsecure) {
    EXPECT_EQ(ph::classify_url("file:///tmp/x"), ph::UrlScheme::File);
    EXPECT_TRUE(ph::is_insecure_scheme("file:///tmp/x"));
}

TEST(PluginsHelpersUrl, UnknownScheme) {
    EXPECT_EQ(ph::classify_url("owner/repo"), ph::UrlScheme::Unknown);
}

// ---------------------------------------------------------------------------
// resolve_git_url
// ---------------------------------------------------------------------------

TEST(PluginsHelpersResolve, PassesThroughHttps) {
    EXPECT_EQ(ph::resolve_git_url("https://github.com/a/b.git"),
              "https://github.com/a/b.git");
}

TEST(PluginsHelpersResolve, PassesThroughSsh) {
    EXPECT_EQ(ph::resolve_git_url("git@github.com:a/b.git"),
              "git@github.com:a/b.git");
}

TEST(PluginsHelpersResolve, ExpandsShorthand) {
    EXPECT_EQ(ph::resolve_git_url("owner/repo"),
              "https://github.com/owner/repo.git");
}

TEST(PluginsHelpersResolve, TrimsSurroundingSlashes) {
    EXPECT_EQ(ph::resolve_git_url("/owner/repo/"),
              "https://github.com/owner/repo.git");
}

TEST(PluginsHelpersResolve, RejectsBareToken) {
    EXPECT_THROW(ph::resolve_git_url("bogus"), std::invalid_argument);
}

TEST(PluginsHelpersResolve, RejectsThreeParts) {
    EXPECT_THROW(ph::resolve_git_url("a/b/c"), std::invalid_argument);
}

// ---------------------------------------------------------------------------
// repo_name_from_url
// ---------------------------------------------------------------------------

TEST(PluginsHelpersRepoName, StripsGitSuffix) {
    EXPECT_EQ(ph::repo_name_from_url("https://github.com/a/my-repo.git"),
              "my-repo");
}

TEST(PluginsHelpersRepoName, SshStyle) {
    EXPECT_EQ(ph::repo_name_from_url("git@github.com:a/repo"), "repo");
}

TEST(PluginsHelpersRepoName, TrailingSlash) {
    EXPECT_EQ(ph::repo_name_from_url("https://host/a/b/"), "b");
}

TEST(PluginsHelpersRepoName, NoPath) {
    EXPECT_EQ(ph::repo_name_from_url("repo.git"), "repo");
}

// ---------------------------------------------------------------------------
// Manifest / env
// ---------------------------------------------------------------------------

TEST(PluginsHelpersManifest, SupportedVersion) {
    EXPECT_TRUE(ph::manifest_version_supported(1));
    EXPECT_FALSE(ph::manifest_version_supported(0));
    EXPECT_FALSE(ph::manifest_version_supported(2));
}

TEST(PluginsHelpersEnv, StringEntry) {
    const auto e {ph::parse_env_entry_string("MY_KEY")};
    ASSERT_TRUE(e.has_value());
    EXPECT_EQ(e->name, "MY_KEY");
    EXPECT_FALSE(e->secret);
}

TEST(PluginsHelpersEnv, EmptyStringEntryRejected) {
    EXPECT_FALSE(ph::parse_env_entry_string("   ").has_value());
}

TEST(PluginsHelpersEnv, DictEntryWithSecret) {
    const auto e {ph::parse_env_entry_dict({
        {"name", "K"},
        {"description", "d"},
        {"url", "u"},
        {"secret", "true"},
    })};
    ASSERT_TRUE(e.has_value());
    EXPECT_EQ(e->name, "K");
    EXPECT_EQ(e->description, "d");
    EXPECT_EQ(e->url, "u");
    EXPECT_TRUE(e->secret);
}

TEST(PluginsHelpersEnv, DictEntryMissingName) {
    EXPECT_FALSE(
        ph::parse_env_entry_dict({{"description", "x"}}).has_value());
}

// ---------------------------------------------------------------------------
// Disabled set
// ---------------------------------------------------------------------------

TEST(PluginsHelpersDisabled, ParseBasic) {
    const auto set {ph::parse_disabled_set("a\nb\n# comment\n\nc\n")};
    EXPECT_EQ(set.size(), 3u);
    EXPECT_EQ(set.count("a"), 1u);
    EXPECT_EQ(set.count("c"), 1u);
}

TEST(PluginsHelpersDisabled, RoundTripSorted) {
    const std::unordered_set<std::string> in {"zeta", "alpha", "mu"};
    const std::string out {ph::serialise_disabled_set(in)};
    EXPECT_EQ(out, "alpha\nmu\nzeta\n");
}

// ---------------------------------------------------------------------------
// Semver + constraints
// ---------------------------------------------------------------------------

TEST(PluginsHelpersSemver, Parse) {
    const auto v {ph::parse_semver("1.2.3")};
    ASSERT_TRUE(v.has_value());
    EXPECT_EQ(*v, std::make_tuple(1, 2, 3));
}

TEST(PluginsHelpersSemver, ParseMajorOnly) {
    const auto v {ph::parse_semver("4")};
    ASSERT_TRUE(v.has_value());
    EXPECT_EQ(*v, std::make_tuple(4, 0, 0));
}

TEST(PluginsHelpersSemver, ParseRejectsAlpha) {
    EXPECT_FALSE(ph::parse_semver("1.a").has_value());
}

TEST(PluginsHelpersSemver, ParseRejectsEmpty) {
    EXPECT_FALSE(ph::parse_semver("").has_value());
    EXPECT_FALSE(ph::parse_semver("1.").has_value());
}

TEST(PluginsHelpersConstraint, ParseGe) {
    const auto c {ph::parse_constraint(">=1.2.3")};
    ASSERT_TRUE(c.has_value());
    EXPECT_EQ(c->first, ">=");
    EXPECT_EQ(c->second, std::make_tuple(1, 2, 3));
}

TEST(PluginsHelpersConstraint, ParseBareIsEq) {
    const auto c {ph::parse_constraint("1.0.0")};
    ASSERT_TRUE(c.has_value());
    EXPECT_EQ(c->first, "==");
}

TEST(PluginsHelpersConstraint, Satisfies) {
    const auto v {std::make_tuple(1, 5, 0)};
    EXPECT_TRUE(ph::satisfies_constraint(v, ">=1.2"));
    EXPECT_TRUE(ph::satisfies_constraint(v, ">1.4"));
    EXPECT_FALSE(ph::satisfies_constraint(v, ">=2.0"));
    EXPECT_TRUE(ph::satisfies_constraint(v, "<=1.5"));
    EXPECT_FALSE(ph::satisfies_constraint(v, "<1.5"));
    EXPECT_TRUE(ph::satisfies_constraint(v, "==1.5.0"));
}

TEST(PluginsHelpersConstraint, MalformedRejected) {
    EXPECT_FALSE(ph::satisfies_constraint(std::make_tuple(1, 0, 0),
                                          ">=garbage"));
}

// ---------------------------------------------------------------------------
// Status / list formatting.
// ---------------------------------------------------------------------------

TEST(PluginsHelpersStatus, Labels) {
    EXPECT_EQ(ph::plugin_status_label(ph::PluginStatus::Enabled), "enabled");
    EXPECT_EQ(ph::plugin_status_label(ph::PluginStatus::Disabled), "disabled");
    EXPECT_EQ(ph::plugin_status_label(ph::PluginStatus::Broken), "broken");
}

TEST(PluginsHelpersFormat, WithDescription) {
    const std::string s {ph::format_list_line(
        "my-plugin", ph::PluginStatus::Enabled, "does stuff")};
    EXPECT_NE(s.find("my-plugin"), std::string::npos);
    EXPECT_NE(s.find("[enabled]"), std::string::npos);
    EXPECT_NE(s.find("does stuff"), std::string::npos);
}

TEST(PluginsHelpersFormat, NoDescription) {
    const std::string s {ph::format_list_line(
        "p", ph::PluginStatus::Disabled, "")};
    EXPECT_EQ(s.find("\xE2\x80\x94"), std::string::npos);
    EXPECT_NE(s.find("[disabled]"), std::string::npos);
}

// ---------------------------------------------------------------------------
// Example-file naming.
// ---------------------------------------------------------------------------

TEST(PluginsHelpersExample, Strip) {
    EXPECT_EQ(ph::example_target_name("config.yaml.example"), "config.yaml");
    EXPECT_EQ(ph::example_target_name("foo.example"), "foo");
}

TEST(PluginsHelpersExample, NonMatching) {
    EXPECT_EQ(ph::example_target_name("config.yaml"), "");
    EXPECT_EQ(ph::example_target_name(".example"), "");
    EXPECT_EQ(ph::example_target_name(""), "");
}

// ---------------------------------------------------------------------------
// Scheme warning.
// ---------------------------------------------------------------------------

TEST(PluginsHelpersSchemeWarn, Http) {
    EXPECT_NE(ph::scheme_warning("http://x").find("insecure"),
              std::string::npos);
}

TEST(PluginsHelpersSchemeWarn, Https) {
    EXPECT_EQ(ph::scheme_warning("https://x"), "");
}

TEST(PluginsHelpersSchemeWarn, File) {
    EXPECT_NE(ph::scheme_warning("file:///tmp").find("insecure"),
              std::string::npos);
}

TEST(PluginsHelpersBanner, Format) {
    const std::string s {
        ph::default_after_install_banner("owner/repo", "/opt/plugins/repo")};
    EXPECT_NE(s.find("owner/repo"), std::string::npos);
    EXPECT_NE(s.find("/opt/plugins/repo"), std::string::npos);
}
