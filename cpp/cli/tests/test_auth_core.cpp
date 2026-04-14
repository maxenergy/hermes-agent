// Unit tests for hermes::cli::auth_core (C++17 port of hermes_cli/auth.py
// foundations).
#include "hermes/cli/auth_core.hpp"

#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <string>

using namespace hermes::cli::auth_core;
namespace fs = std::filesystem;

namespace {

class AuthCoreTest : public ::testing::Test {
protected:
    fs::path tmp;
    std::string saved_home;
    std::string saved_hermes_home;
    std::string saved_trace;

    void SetUp() override {
        auto base = fs::temp_directory_path() / "hermes_auth_core_test";
        fs::create_directories(base);
        tmp = base / ("t" +
                      std::to_string(::getpid()) + "_" +
                      ::testing::UnitTest::GetInstance()
                          ->current_test_info()
                          ->name());
        fs::create_directories(tmp);
        saved_home = getenv("HOME") ? getenv("HOME") : "";
        saved_hermes_home = getenv("HERMES_HOME") ? getenv("HERMES_HOME") : "";
        saved_trace = getenv("HERMES_OAUTH_TRACE") ? getenv("HERMES_OAUTH_TRACE") : "";
        setenv("HERMES_HOME", tmp.c_str(), 1);
        unsetenv("HERMES_OAUTH_TRACE");
    }

    void TearDown() override {
        std::error_code ec;
        fs::remove_all(tmp, ec);
        auto restore = [](const char* k, const std::string& v) {
            if (v.empty()) unsetenv(k);
            else setenv(k, v.c_str(), 1);
        };
        restore("HOME", saved_home);
        restore("HERMES_HOME", saved_hermes_home);
        restore("HERMES_OAUTH_TRACE", saved_trace);
    }
};

TEST_F(AuthCoreTest, AuthErrorFormattingPlainRuntimeError) {
    std::runtime_error err("oops");
    EXPECT_EQ(format_auth_error(err), "oops");
}

TEST_F(AuthCoreTest, AuthErrorFormattingReloginRequired) {
    AuthError e("Token expired", "anthropic", std::nullopt, true);
    auto msg = format_auth_error(e);
    EXPECT_TRUE(msg.find("re-authenticate") != std::string::npos);
}

TEST_F(AuthCoreTest, AuthErrorFormattingSubscriptionRequired) {
    AuthError e("no sub", "nous", std::string("subscription_required"), false);
    auto msg = format_auth_error(e);
    EXPECT_TRUE(msg.find("subscription") != std::string::npos);
}

TEST_F(AuthCoreTest, AuthErrorFormattingInsufficientCredits) {
    AuthError e("empty", "nous", std::string("insufficient_credits"), false);
    auto msg = format_auth_error(e);
    EXPECT_TRUE(msg.find("credits") != std::string::npos);
}

TEST_F(AuthCoreTest, AuthErrorFormattingTemporarilyUnavailable) {
    AuthError e("flaky", "nous", std::string("temporarily_unavailable"), false);
    auto msg = format_auth_error(e);
    EXPECT_TRUE(msg.find("retry") != std::string::npos);
}

TEST_F(AuthCoreTest, TokenFingerprintStable) {
    auto fp = token_fingerprint("super-secret-token-abc123");
    ASSERT_TRUE(fp.has_value());
    EXPECT_EQ(fp->size(), 12u);
    auto fp2 = token_fingerprint(" super-secret-token-abc123 ");
    ASSERT_TRUE(fp2.has_value());
    EXPECT_EQ(*fp, *fp2);  // stripped before hashing
}

TEST_F(AuthCoreTest, TokenFingerprintEmptyRejected) {
    EXPECT_FALSE(token_fingerprint("").has_value());
    EXPECT_FALSE(token_fingerprint("   ").has_value());
}

TEST_F(AuthCoreTest, OAuthTraceEnabledReadsEnv) {
    setenv("HERMES_OAUTH_TRACE", "1", 1);
    EXPECT_TRUE(oauth_trace_enabled());
    setenv("HERMES_OAUTH_TRACE", "yes", 1);
    EXPECT_TRUE(oauth_trace_enabled());
    setenv("HERMES_OAUTH_TRACE", "off", 1);
    EXPECT_FALSE(oauth_trace_enabled());
    unsetenv("HERMES_OAUTH_TRACE");
    EXPECT_FALSE(oauth_trace_enabled());
}

TEST_F(AuthCoreTest, HasUsableSecret) {
    EXPECT_TRUE(has_usable_secret(std::string("abcd")));
    EXPECT_FALSE(has_usable_secret(std::string("abc")));
    EXPECT_FALSE(has_usable_secret(std::string("")));
    EXPECT_FALSE(has_usable_secret(std::string("  ")));
    EXPECT_TRUE(has_usable_secret(nlohmann::json("abcdef")));
    EXPECT_FALSE(has_usable_secret(nlohmann::json(123)));
    EXPECT_FALSE(has_usable_secret(nlohmann::json()));
}

TEST_F(AuthCoreTest, ParseIsoTimestampUtc) {
    auto ts = parse_iso_timestamp(std::string("2025-04-13T12:00:00Z"));
    ASSERT_TRUE(ts.has_value());
    EXPECT_NEAR(*ts, 1744545600.0, 1.0);
}

TEST_F(AuthCoreTest, ParseIsoTimestampOffset) {
    // 2025-04-13T14:00:00+02:00 == 2025-04-13T12:00:00Z
    auto ts1 = parse_iso_timestamp(std::string("2025-04-13T14:00:00+02:00"));
    auto ts2 = parse_iso_timestamp(std::string("2025-04-13T12:00:00Z"));
    ASSERT_TRUE(ts1.has_value());
    ASSERT_TRUE(ts2.has_value());
    EXPECT_NEAR(*ts1, *ts2, 1.0);
}

TEST_F(AuthCoreTest, ParseIsoTimestampFraction) {
    auto ts = parse_iso_timestamp(std::string("2025-04-13T12:00:00.500Z"));
    ASSERT_TRUE(ts.has_value());
    EXPECT_NEAR(*ts, 1744545600.5, 0.01);
}

TEST_F(AuthCoreTest, ParseIsoTimestampRejectsGarbage) {
    EXPECT_FALSE(parse_iso_timestamp(std::string("not a date")).has_value());
    EXPECT_FALSE(parse_iso_timestamp(std::string("")).has_value());
    EXPECT_FALSE(parse_iso_timestamp(nlohmann::json(42)).has_value());
}

TEST_F(AuthCoreTest, IsExpiringPastAlwaysTrue) {
    EXPECT_TRUE(is_expiring("2000-01-01T00:00:00Z"));
}

TEST_F(AuthCoreTest, IsExpiringFutureFalse) {
    EXPECT_FALSE(is_expiring("2099-01-01T00:00:00Z"));
}

TEST_F(AuthCoreTest, IsExpiringMissingIsTrue) {
    EXPECT_TRUE(is_expiring(nlohmann::json()));
    EXPECT_TRUE(is_expiring(nlohmann::json("garbage")));
}

TEST_F(AuthCoreTest, CoerceTtlSeconds) {
    EXPECT_EQ(coerce_ttl_seconds(3600), 3600);
    EXPECT_EQ(coerce_ttl_seconds("60"), 60);
    EXPECT_EQ(coerce_ttl_seconds(-1), 0);
    EXPECT_EQ(coerce_ttl_seconds("not-a-number"), 0);
    EXPECT_EQ(coerce_ttl_seconds(nlohmann::json()), 0);
}

TEST_F(AuthCoreTest, OptionalBaseUrlStripsTrailingSlash) {
    auto u = optional_base_url(nlohmann::json("https://api.example.com/"));
    ASSERT_TRUE(u.has_value());
    EXPECT_EQ(*u, "https://api.example.com");
    EXPECT_FALSE(optional_base_url(nlohmann::json("")).has_value());
    EXPECT_FALSE(optional_base_url(nlohmann::json()).has_value());
}

TEST_F(AuthCoreTest, DecodeJwtClaims) {
    // {"sub":"user","exp":99999999999}
    // header.payload.signature — header and signature are irrelevant.
    std::string token =
        "eyJhbGciOiJIUzI1NiJ9."
        "eyJzdWIiOiJ1c2VyIiwiZXhwIjo5OTk5OTk5OTk5OX0."
        "ignored";
    auto claims = decode_jwt_claims(token);
    ASSERT_TRUE(claims.is_object());
    EXPECT_EQ(claims["sub"], "user");
    EXPECT_TRUE(claims.contains("exp"));
}

TEST_F(AuthCoreTest, DecodeJwtRejectsMalformed) {
    EXPECT_TRUE(decode_jwt_claims("not-a-jwt").empty());
    EXPECT_TRUE(decode_jwt_claims("one.two").empty());
    EXPECT_TRUE(decode_jwt_claims("one.two.three.four").empty());
}

TEST_F(AuthCoreTest, CodexTokenExpiry) {
    // exp = 1 (far past)
    std::string past_token =
        "h.eyJleHAiOjF9.s";
    EXPECT_TRUE(codex_access_token_is_expiring(past_token, 60));

    // exp = 9999999999 (far future)
    std::string future_token =
        "h.eyJleHAiOjk5OTk5OTk5OTk5fQ.s";
    EXPECT_FALSE(codex_access_token_is_expiring(future_token, 60));
}

TEST_F(AuthCoreTest, AuthStoreRoundTrip) {
    nlohmann::json store;
    store["active_provider"] = "anthropic";
    store["providers"] = {{"anthropic", {{"token", "abc"}}}};
    auto path = save_auth_store(store);
    EXPECT_TRUE(fs::exists(path));
    auto loaded = load_auth_store();
    EXPECT_EQ(loaded["active_provider"], "anthropic");
    EXPECT_EQ(loaded["providers"]["anthropic"]["token"], "abc");
}

TEST_F(AuthCoreTest, ProviderSlotHelpers) {
    nlohmann::json store;
    save_provider_state(store, "openai", {{"token", "xyz"}});
    auto s = load_provider_state(store, "openai");
    ASSERT_TRUE(s.has_value());
    EXPECT_EQ((*s)["token"], "xyz");
    EXPECT_FALSE(load_provider_state(store, "absent").has_value());
}

TEST_F(AuthCoreTest, GetActiveProviderRoundTrip) {
    nlohmann::json store;
    store["active_provider"] = "openrouter";
    save_auth_store(store);
    auto p = get_active_provider();
    ASSERT_TRUE(p.has_value());
    EXPECT_EQ(*p, "openrouter");
}

TEST_F(AuthCoreTest, ClearProviderAuth) {
    nlohmann::json store;
    store["active_provider"] = "anthropic";
    store["providers"] = {{"anthropic", {{"token", "a"}}},
                          {"openai", {{"token", "o"}}}};
    save_auth_store(store);

    EXPECT_TRUE(clear_provider_auth("anthropic"));
    auto s2 = load_auth_store();
    EXPECT_FALSE(s2["providers"].contains("anthropic"));
    EXPECT_TRUE(s2["providers"].contains("openai"));
    EXPECT_FALSE(s2.contains("active_provider"));

    EXPECT_TRUE(clear_provider_auth());
    auto s3 = load_auth_store();
    EXPECT_TRUE(!s3.contains("providers") ||
                s3["providers"].empty());
}

TEST_F(AuthCoreTest, CredentialPoolRoundTrip) {
    std::vector<nlohmann::json> entries;
    entries.push_back({{"id", "k1"}, {"token_fingerprint", "aaa"}});
    entries.push_back({{"id", "k2"}, {"token_fingerprint", "bbb"}});
    write_credential_pool("anthropic", entries);

    auto slice = read_credential_pool("anthropic");
    ASSERT_TRUE(slice.is_array());
    EXPECT_EQ(slice.size(), 2u);
    EXPECT_EQ(slice[0]["id"], "k1");

    auto all = read_credential_pool();
    EXPECT_TRUE(all.contains("anthropic"));
}

TEST_F(AuthCoreTest, SuppressCredentialSource) {
    EXPECT_FALSE(is_source_suppressed("anthropic", "env:ANTHROPIC_API_KEY"));
    suppress_credential_source("anthropic", "env:ANTHROPIC_API_KEY");
    EXPECT_TRUE(is_source_suppressed("anthropic", "env:ANTHROPIC_API_KEY"));
    // Idempotent (no duplicate).
    suppress_credential_source("anthropic", "env:ANTHROPIC_API_KEY");
    auto store = load_auth_store();
    EXPECT_EQ(store["suppressed_sources"]["anthropic"].size(), 1u);
}

TEST_F(AuthCoreTest, ProviderRegistryLookup) {
    const auto& reg = provider_registry();
    EXPECT_FALSE(reg.empty());
    EXPECT_TRUE(reg.find("anthropic") != reg.end());
    EXPECT_EQ(reg.at("anthropic").auth_type, kAuthTypeApiKey);
    EXPECT_EQ(reg.at("copilot").auth_type, kAuthTypeOAuth);
    EXPECT_EQ(reg.at("nous").auth_type, kAuthTypeManaged);
}

TEST_F(AuthCoreTest, FindProviderCaseInsensitive) {
    EXPECT_TRUE(find_provider("Anthropic") != nullptr);
    EXPECT_TRUE(find_provider("  ANTHROPIC ") != nullptr);
    EXPECT_TRUE(find_provider("nonsense") == nullptr);
    EXPECT_TRUE(find_provider("") == nullptr);
}

TEST_F(AuthCoreTest, ExplicitlyConfiguredByActiveProvider) {
    nlohmann::json store;
    store["active_provider"] = "anthropic";
    save_auth_store(store);
    // Inject empty env to avoid picking up the host's real env vars.
    auto empty_env = [](const std::string&) -> std::string { return {}; };
    EXPECT_TRUE(is_provider_explicitly_configured("anthropic", "", empty_env));
    EXPECT_FALSE(is_provider_explicitly_configured("openai", "", empty_env));
}

TEST_F(AuthCoreTest, ExplicitlyConfiguredByConfigProvider) {
    auto empty_env = [](const std::string&) -> std::string { return {}; };
    EXPECT_TRUE(
        is_provider_explicitly_configured("openai", "openai", empty_env));
    EXPECT_FALSE(
        is_provider_explicitly_configured("openai", "anthropic", empty_env));
}

TEST_F(AuthCoreTest, ExplicitlyConfiguredByEnvIgnoresImplicit) {
    auto env = [](const std::string& key) -> std::string {
        if (key == "ANTHROPIC_API_KEY") return "sk-ant-abcdef1234";
        if (key == "CLAUDE_CODE_OAUTH_TOKEN") return "external-token";
        return {};
    };
    EXPECT_TRUE(is_provider_explicitly_configured("anthropic", "", env));

    // Env without valid ANTHROPIC_API_KEY — only the implicit one set.
    auto env_implicit_only = [](const std::string& key) -> std::string {
        if (key == "CLAUDE_CODE_OAUTH_TOKEN") return "external-token";
        return {};
    };
    EXPECT_FALSE(is_provider_explicitly_configured("anthropic", "",
                                                    env_implicit_only));
}

TEST_F(AuthCoreTest, DeactivateProvider) {
    nlohmann::json store;
    store["active_provider"] = "anthropic";
    store["providers"] = {{"anthropic", {{"token", "a"}}}};
    save_auth_store(store);
    deactivate_provider();
    auto s = load_auth_store();
    EXPECT_FALSE(s.contains("active_provider"));
    // Provider state is preserved.
    EXPECT_TRUE(s["providers"].contains("anthropic"));
}

}  // namespace
