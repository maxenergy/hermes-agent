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
    EXPECT_EQ(migrated["terminal"]["backend"].get<std::string>(), "docker");
    EXPECT_EQ(migrated["_config_version"].get<int>(), hc::kCurrentConfigVersion);
}

TEST(MigrateConfig, DoesNotDowngradeVersion) {
    // A future config coming in with a higher version must not be
    // rolled back.
    nlohmann::json cfg = {{"_config_version", 99}};
    auto migrated = hc::migrate_config(cfg);
    EXPECT_EQ(migrated["_config_version"].get<int>(), 99);
}
