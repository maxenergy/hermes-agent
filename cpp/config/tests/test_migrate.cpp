#include "hermes/config/default_config.hpp"
#include "hermes/config/loader.hpp"

#include <gtest/gtest.h>

namespace hc = hermes::config;

TEST(MigrateConfig, EmptyConfigGetsVersionStamp) {
    nlohmann::json cfg = nlohmann::json::object();
    auto migrated = hc::migrate_config(cfg);
    EXPECT_EQ(migrated["_config_version"].get<int>(), hc::kCurrentConfigVersion);
}

TEST(MigrateConfig, PreservesUserFields) {
    nlohmann::json cfg = {
        {"model", "claude-sonnet"},
        {"terminal", {{"backend", "docker"}}},
        {"_config_version", 1},
    };
    auto migrated = hc::migrate_config(cfg);
    EXPECT_EQ(migrated["model"].get<std::string>(), "claude-sonnet");
    // User's existing terminal.backend="docker" is preserved (not overwritten).
    EXPECT_EQ(migrated["terminal"]["backend"].get<std::string>(), "docker");
    EXPECT_EQ(migrated["_config_version"].get<int>(), hc::kCurrentConfigVersion);
}

TEST(MigrateConfig, DoesNotDowngradeVersion) {
    nlohmann::json cfg = {{"_config_version", 99}};
    auto migrated = hc::migrate_config(cfg);
    EXPECT_EQ(migrated["_config_version"].get<int>(), 99);
}

TEST(MigrateConfig, V1ToV5FullMigration) {
    // Start with a v1 config that has an api_key (old field).
    nlohmann::json cfg = {
        {"_config_version", 1},
        {"model", "gpt-4o"},
        {"api_key", "sk-test-123"},
    };
    auto migrated = hc::migrate_config(cfg);

    // Should now be at the current version (v6).
    EXPECT_EQ(migrated["_config_version"].get<int>(), hc::kCurrentConfigVersion);

    // v1->v2: terminal.backend added.
    EXPECT_EQ(migrated["terminal"]["backend"].get<std::string>(), "local");

    // v2->v3: api_key renamed to provider_api_key.
    EXPECT_FALSE(migrated.contains("api_key"));
    EXPECT_EQ(migrated["provider_api_key"].get<std::string>(), "sk-test-123");

    // v3->v4: display.skin added.
    EXPECT_EQ(migrated["display"]["skin"].get<std::string>(), "default");

    // v4->v5: web.backend and tts.provider added.
    EXPECT_EQ(migrated["web"]["backend"].get<std::string>(), "exa");
    EXPECT_EQ(migrated["tts"]["provider"].get<std::string>(), "edge");

    // Original model preserved.
    EXPECT_EQ(migrated["model"].get<std::string>(), "gpt-4o");
}

TEST(MigrateConfig, V3ToV5SkipsEarlyMigrations) {
    nlohmann::json cfg = {
        {"_config_version", 3},
        {"terminal", {{"backend", "docker"}}},
        {"display", {{"skin", "ocean"}}},  // user already set skin
    };
    auto migrated = hc::migrate_config(cfg);

    EXPECT_EQ(migrated["_config_version"].get<int>(), hc::kCurrentConfigVersion);
    // Pre-existing terminal.backend preserved.
    EXPECT_EQ(migrated["terminal"]["backend"].get<std::string>(), "docker");
    // Pre-existing display.skin preserved (not overwritten by migration).
    EXPECT_EQ(migrated["display"]["skin"].get<std::string>(), "ocean");
    // v4->v5 additions.
    EXPECT_EQ(migrated["web"]["backend"].get<std::string>(), "exa");
    EXPECT_EQ(migrated["tts"]["provider"].get<std::string>(), "edge");
}

TEST(MigrateConfig, V2ToV3RenamesApiKey) {
    nlohmann::json cfg = {
        {"_config_version", 2},
        {"api_key", "my-secret-key"},
    };
    auto migrated = hc::migrate_config(cfg);

    EXPECT_FALSE(migrated.contains("api_key"));
    EXPECT_EQ(migrated["provider_api_key"].get<std::string>(), "my-secret-key");
}

TEST(MigrateConfig, V2ToV3NoApiKeyNoOp) {
    // If there's no api_key, the rename is a no-op.
    nlohmann::json cfg = {
        {"_config_version", 2},
        {"provider_api_key", "already-renamed"},
    };
    auto migrated = hc::migrate_config(cfg);

    EXPECT_EQ(migrated["provider_api_key"].get<std::string>(), "already-renamed");
    EXPECT_FALSE(migrated.contains("api_key"));
}

// ---------------------------------------------------------------------------
// v6 -> v14 coverage (no-op version bumps + schema changes)
// ---------------------------------------------------------------------------

TEST(MigrateConfig, V6WalksStraightToCurrent) {
    // A v6 config with no legacy fields should be stamped forward without
    // adding any schema sections that didn't already exist.
    nlohmann::json cfg = {{"_config_version", 6}};
    auto migrated = hc::migrate_config(cfg);
    EXPECT_EQ(migrated["_config_version"].get<int>(), 14);
    EXPECT_EQ(migrated["_config_version"].get<int>(), hc::kCurrentConfigVersion);
    // No legacy list → no providers dict added by migration.
    EXPECT_FALSE(migrated.contains("providers"));
    // No legacy stt.model → no nested stt.local injected.
    EXPECT_FALSE(migrated.contains("stt"));
}

TEST(MigrateConfig, V11ToV12MigratesCustomProviders) {
    nlohmann::json cfg = {
        {"_config_version", 11},
        {"custom_providers", nlohmann::json::array({
            {
                {"name", "My Endpoint"},
                {"base_url", "https://example.com/v1"},
                {"api_key", "sk-endpoint"},
                {"model", "my-model"},
                {"api_mode", "responses"},
            },
            {
                {"name", "No Key Host"},
                {"url", "https://free.example.net/v1"},
                {"api_key", "no-key-required"},
            },
        })},
    };
    auto out = hc::migrate_config(cfg);

    EXPECT_FALSE(out.contains("custom_providers"));
    ASSERT_TRUE(out.contains("providers"));
    ASSERT_TRUE(out["providers"].is_object());
    ASSERT_TRUE(out["providers"].contains("my-endpoint"));
    const auto& me = out["providers"]["my-endpoint"];
    EXPECT_EQ(me["api"].get<std::string>(), "https://example.com/v1");
    EXPECT_EQ(me["api_key"].get<std::string>(), "sk-endpoint");
    EXPECT_EQ(me["default_model"].get<std::string>(), "my-model");
    EXPECT_EQ(me["transport"].get<std::string>(), "responses");

    ASSERT_TRUE(out["providers"].contains("no-key-host"));
    const auto& nk = out["providers"]["no-key-host"];
    EXPECT_EQ(nk["api"].get<std::string>(), "https://free.example.net/v1");
    // Placeholder api_key ("no-key-required") is dropped.
    EXPECT_FALSE(nk.contains("api_key"));
}

TEST(MigrateConfig, V11SkipsEntryWithNoUrl) {
    nlohmann::json cfg = {
        {"_config_version", 11},
        {"custom_providers", nlohmann::json::array({
            {{"name", "nopey"}},  // no base_url/url → skipped
        })},
    };
    auto out = hc::migrate_config(cfg);
    // custom_providers stays (migrated_count == 0 → don't erase).
    EXPECT_TRUE(out.contains("custom_providers"));
    EXPECT_EQ(out["_config_version"].get<int>(), hc::kCurrentConfigVersion);
}

TEST(MigrateConfig, V13ToV14MigratesLocalSttModel) {
    nlohmann::json cfg = {
        {"_config_version", 13},
        {"stt", {
            {"provider", "local"},
            {"model", "large-v3"},
        }},
    };
    auto out = hc::migrate_config(cfg);
    EXPECT_FALSE(out["stt"].contains("model"));
    ASSERT_TRUE(out["stt"].contains("local"));
    EXPECT_EQ(out["stt"]["local"]["model"].get<std::string>(), "large-v3");
}

TEST(MigrateConfig, V13ToV14DropsOpenAiNameFromLocalProvider) {
    // "whisper-1" is not a faster-whisper model — drop it rather than
    // poisoning the local section.
    nlohmann::json cfg = {
        {"_config_version", 13},
        {"stt", {
            {"provider", "local"},
            {"model", "whisper-1"},
        }},
    };
    auto out = hc::migrate_config(cfg);
    EXPECT_FALSE(out["stt"].contains("model"));
    // No local.model injected — DEFAULT_CONFIG's "base" takes effect at load.
    if (out["stt"].contains("local") && out["stt"]["local"].is_object()) {
        EXPECT_FALSE(out["stt"]["local"].contains("model"));
    }
}

TEST(MigrateConfig, V13ToV14MigratesCloudSttModel) {
    nlohmann::json cfg = {
        {"_config_version", 13},
        {"stt", {
            {"provider", "openai"},
            {"model", "whisper-1"},
        }},
    };
    auto out = hc::migrate_config(cfg);
    EXPECT_FALSE(out["stt"].contains("model"));
    ASSERT_TRUE(out["stt"].contains("openai"));
    EXPECT_EQ(out["stt"]["openai"]["model"].get<std::string>(), "whisper-1");
}

TEST(MigrateConfig, V13ToV14PreservesExistingNestedModel) {
    nlohmann::json cfg = {
        {"_config_version", 13},
        {"stt", {
            {"provider", "openai"},
            {"model", "whisper-1"},
            {"openai", {{"model", "gpt-4o-transcribe"}}},
        }},
    };
    auto out = hc::migrate_config(cfg);
    EXPECT_FALSE(out["stt"].contains("model"));
    EXPECT_EQ(out["stt"]["openai"]["model"].get<std::string>(), "gpt-4o-transcribe");
}

TEST(MigrateConfig, V6ToV14FullWalkWithAllLegacyFields) {
    // Exercise v6 → v14 in a single call with both legacy features set.
    nlohmann::json cfg = {
        {"_config_version", 6},
        {"custom_providers", nlohmann::json::array({
            {
                {"name", "Old Host"},
                {"base_url", "https://old.example.com/v1"},
            },
        })},
        {"stt", {
            {"provider", "local"},
            {"model", "medium"},
        }},
    };
    auto out = hc::migrate_config(cfg);
    EXPECT_EQ(out["_config_version"].get<int>(), 14);
    EXPECT_FALSE(out.contains("custom_providers"));
    ASSERT_TRUE(out["providers"].contains("old-host"));
    EXPECT_EQ(out["providers"]["old-host"]["api"].get<std::string>(),
              "https://old.example.com/v1");
    EXPECT_FALSE(out["stt"].contains("model"));
    EXPECT_EQ(out["stt"]["local"]["model"].get<std::string>(), "medium");
}
