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

    // Should now be at v5.
    EXPECT_EQ(migrated["_config_version"].get<int>(), 5);

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

    EXPECT_EQ(migrated["_config_version"].get<int>(), 5);
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
