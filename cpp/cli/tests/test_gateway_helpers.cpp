// Tests for hermes::cli::gateway_helpers.
#include "hermes/cli/gateway_helpers.hpp"

#include <gtest/gtest.h>

#include <set>

using namespace hermes::cli::gateway_helpers;
namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// Profile-name validator.
// ---------------------------------------------------------------------------

TEST(GatewayHelpers_ProfileName, AcceptsLowerAlnumDashUnderscore) {
    EXPECT_TRUE(is_valid_profile_name("coder"));
    EXPECT_TRUE(is_valid_profile_name("coder-2"));
    EXPECT_TRUE(is_valid_profile_name("a1_b2-c3"));
    EXPECT_TRUE(is_valid_profile_name("0valid"));
}

TEST(GatewayHelpers_ProfileName, RejectsBadChars) {
    EXPECT_FALSE(is_valid_profile_name("Coder"));      // upper
    EXPECT_FALSE(is_valid_profile_name(""));            // empty
    EXPECT_FALSE(is_valid_profile_name("-foo"));        // leading dash
    EXPECT_FALSE(is_valid_profile_name("foo bar"));    // space
    EXPECT_FALSE(is_valid_profile_name("foo/bar"));    // slash
    EXPECT_FALSE(is_valid_profile_name(std::string(70, 'a')));  // too long
}

// ---------------------------------------------------------------------------
// Profile suffix.
// ---------------------------------------------------------------------------

TEST(GatewayHelpers_ProfileSuffix, DefaultRootEmpty) {
    EXPECT_EQ(profile_suffix("/home/u/.hermes", "/home/u/.hermes"), "");
}

TEST(GatewayHelpers_ProfileSuffix, NamedProfile) {
    EXPECT_EQ(profile_suffix("/home/u/.hermes/profiles/coder",
                             "/home/u/.hermes"),
              "coder");
}

TEST(GatewayHelpers_ProfileSuffix, NestedProfilePathHashes) {
    auto s = profile_suffix("/home/u/.hermes/profiles/coder/inner",
                            "/home/u/.hermes");
    EXPECT_EQ(s.size(), 8u);
    // hex chars only
    for (char c : s) {
        EXPECT_TRUE(std::isxdigit(static_cast<unsigned char>(c)));
    }
}

TEST(GatewayHelpers_ProfileSuffix, CustomPathHashes) {
    auto s = profile_suffix("/opt/hermes-data", "/home/u/.hermes");
    EXPECT_EQ(s.size(), 8u);
}

TEST(GatewayHelpers_ProfileSuffix, DockerLikePathHashes) {
    auto s = profile_suffix("/opt/data", "/home/u/.hermes");
    auto t = profile_suffix("/opt/data", "/home/u/.hermes");
    EXPECT_EQ(s, t);  // stable
    auto u = profile_suffix("/opt/other", "/home/u/.hermes");
    EXPECT_NE(s, u);  // distinct
}

// ---------------------------------------------------------------------------
// Profile arg.
// ---------------------------------------------------------------------------

TEST(GatewayHelpers_ProfileArg, DefaultRootEmpty) {
    EXPECT_EQ(profile_arg("/home/u/.hermes", "/home/u/.hermes"), "");
}

TEST(GatewayHelpers_ProfileArg, NamedProfileEmitsArg) {
    EXPECT_EQ(profile_arg("/home/u/.hermes/profiles/coder",
                          "/home/u/.hermes"),
              "--profile coder");
}

TEST(GatewayHelpers_ProfileArg, CustomPathNoArg) {
    EXPECT_EQ(profile_arg("/opt/data", "/home/u/.hermes"), "");
}

// ---------------------------------------------------------------------------
// Service name + launchd label.
// ---------------------------------------------------------------------------

TEST(GatewayHelpers_ServiceName, DefaultIsBare) {
    EXPECT_EQ(service_name("/home/u/.hermes", "/home/u/.hermes"),
              "hermes-gateway");
}

TEST(GatewayHelpers_ServiceName, NamedProfile) {
    EXPECT_EQ(service_name("/home/u/.hermes/profiles/coder",
                           "/home/u/.hermes"),
              "hermes-gateway-coder");
}

TEST(GatewayHelpers_ServiceName, HashedSuffix) {
    auto n = service_name("/opt/data", "/home/u/.hermes");
    EXPECT_TRUE(n.rfind("hermes-gateway-", 0) == 0);
    EXPECT_EQ(n.size(), std::string("hermes-gateway-").size() + 8);
}

TEST(GatewayHelpers_LaunchdLabel, DefaultIsBare) {
    EXPECT_EQ(launchd_label("/home/u/.hermes", "/home/u/.hermes"),
              "ai.nous.hermes.gateway");
}

TEST(GatewayHelpers_LaunchdLabel, NamedProfile) {
    EXPECT_EQ(launchd_label("/home/u/.hermes/profiles/coder",
                            "/home/u/.hermes"),
              "ai.nous.hermes.gateway.coder");
}

// ---------------------------------------------------------------------------
// Unit paths.
// ---------------------------------------------------------------------------

TEST(GatewayHelpers_UnitPaths, UserUnitPath) {
    EXPECT_EQ(user_systemd_unit_path("/home/u/.config",
                                     "/home/u/.hermes/profiles/coder",
                                     "/home/u/.hermes"),
              fs::path("/home/u/.config/systemd/user/hermes-gateway-coder.service"));
}

TEST(GatewayHelpers_UnitPaths, SystemUnitPath) {
    EXPECT_EQ(system_systemd_unit_path("/home/u/.hermes", "/home/u/.hermes"),
              fs::path("/etc/systemd/system/hermes-gateway.service"));
}

// ---------------------------------------------------------------------------
// Path remapping.
// ---------------------------------------------------------------------------

TEST(GatewayHelpers_Remap, RemapsUnderHome) {
    auto r = remap_path_for_user("/root/.hermes/hermes-agent",
                                 "/root", "/home/alice");
    EXPECT_EQ(r, "/home/alice/.hermes/hermes-agent");
}

TEST(GatewayHelpers_Remap, NoOpOutsideHome) {
    EXPECT_EQ(remap_path_for_user("/opt/hermes", "/root", "/home/alice"),
              "/opt/hermes");
}

TEST(GatewayHelpers_HermesHomeRemap, DefaultPath) {
    EXPECT_EQ(hermes_home_for_target_user("/root/.hermes", "/root",
                                          "/home/alice"),
              fs::path("/home/alice/.hermes"));
}

TEST(GatewayHelpers_HermesHomeRemap, ProfileSubdir) {
    EXPECT_EQ(hermes_home_for_target_user(
                  "/root/.hermes/profiles/coder", "/root", "/home/alice"),
              fs::path("/home/alice/.hermes/profiles/coder"));
}

TEST(GatewayHelpers_HermesHomeRemap, CustomKeptAsIs) {
    EXPECT_EQ(hermes_home_for_target_user("/opt/custom", "/root", "/home/alice"),
              fs::path("/opt/custom"));
}

// ---------------------------------------------------------------------------
// build_user_local_paths.
// ---------------------------------------------------------------------------

TEST(GatewayHelpers_BuildPaths, ReturnsExistingNotInList) {
    std::set<fs::path> exists = {"/h/.local/bin", "/h/go/bin"};
    auto check = [&](const fs::path& p) { return exists.count(p) > 0; };
    auto out = build_user_local_paths("/h", {"/h/.cargo/bin"}, check);
    ASSERT_EQ(out.size(), 2u);
    EXPECT_EQ(out[0], "/h/.local/bin");
    EXPECT_EQ(out[1], "/h/go/bin");
}

TEST(GatewayHelpers_BuildPaths, SkipsAlreadyPresent) {
    std::set<fs::path> exists = {"/h/.local/bin"};
    auto check = [&](const fs::path& p) { return exists.count(p) > 0; };
    auto out = build_user_local_paths("/h", {"/h/.local/bin"}, check);
    EXPECT_TRUE(out.empty());
}

TEST(GatewayHelpers_BuildPaths, OrderedByCandidateOrder) {
    auto check = [](const fs::path&) { return true; };
    auto out = build_user_local_paths("/h", {}, check);
    ASSERT_EQ(out.size(), 4u);
    EXPECT_EQ(out[0], "/h/.local/bin");
    EXPECT_EQ(out[1], "/h/.cargo/bin");
    EXPECT_EQ(out[2], "/h/go/bin");
    EXPECT_EQ(out[3], "/h/.npm-global/bin");
}

// ---------------------------------------------------------------------------
// Service-definition normaliser.
// ---------------------------------------------------------------------------

TEST(GatewayHelpers_Normalize, StripsTrailingWhitespace) {
    // Python `text.strip()` strips outer whitespace including the leading
    // indent on the first line, then trims each line's trailing spaces.
    EXPECT_EQ(normalize_service_definition("  line1   \n  line2 \n"),
              "line1\n  line2");
}

TEST(GatewayHelpers_Normalize, EqualForLogicallyEqual) {
    auto a = normalize_service_definition(
        "[Unit]\nDescription=svc   \n[Service]\nExecStart=foo\n\n");
    auto b = normalize_service_definition(
        "  [Unit]\nDescription=svc\n[Service]\nExecStart=foo");
    EXPECT_EQ(a, b);
}

// ---------------------------------------------------------------------------
// parse_restart_drain_timeout.
// ---------------------------------------------------------------------------

TEST(GatewayHelpers_DrainTimeout, ParsesPositive) {
    auto v = parse_restart_drain_timeout("90");
    ASSERT_TRUE(v.has_value());
    EXPECT_EQ(*v, 90);
}

TEST(GatewayHelpers_DrainTimeout, EmptyOrInvalidNullopt) {
    EXPECT_FALSE(parse_restart_drain_timeout("").has_value());
    EXPECT_FALSE(parse_restart_drain_timeout("abc").has_value());
    EXPECT_FALSE(parse_restart_drain_timeout("12abc").has_value());
}

TEST(GatewayHelpers_DrainTimeout, RejectsZeroAndNegative) {
    EXPECT_FALSE(parse_restart_drain_timeout("0").has_value());
    EXPECT_FALSE(parse_restart_drain_timeout("-5").has_value());
}

TEST(GatewayHelpers_DrainTimeout, TrimsWhitespace) {
    auto v = parse_restart_drain_timeout("  42  ");
    ASSERT_TRUE(v.has_value());
    EXPECT_EQ(*v, 42);
}

// ---------------------------------------------------------------------------
// Allowlist split.
// ---------------------------------------------------------------------------

TEST(GatewayHelpers_Split, EmptyInput) {
    EXPECT_TRUE(split_allowlist("").empty());
}

TEST(GatewayHelpers_Split, BasicCsv) {
    auto v = split_allowlist("a,b,c");
    ASSERT_EQ(v.size(), 3u);
    EXPECT_EQ(v[1], "b");
}

TEST(GatewayHelpers_Split, TrimsAndDropsBlanks) {
    auto v = split_allowlist("  a , , b ,,c");
    ASSERT_EQ(v.size(), 3u);
    EXPECT_EQ(v[0], "a");
    EXPECT_EQ(v[1], "b");
    EXPECT_EQ(v[2], "c");
}
