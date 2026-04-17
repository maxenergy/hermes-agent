// Per-version config migration fixtures.
//
// Each TEST fabricates a synthetic `<HERMES_HOME>/config.yaml` pinned at
// an older `_config_version` and verifies that `load_cli_config()` +
// `migrate_config()` walks it forward to `kCurrentConfigVersion`, applying
// every intermediate transformation.  These complement the unit tests in
// `test_migrate.cpp` (which operate directly on JSON) by exercising the
// YAML parser and the on-disk file path — the surface users actually hit.

#include "hermes/config/default_config.hpp"
#include "hermes/config/loader.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <string>

namespace hc = hermes::config;
namespace fs = std::filesystem;

namespace {

class TempHermesHome {
public:
    TempHermesHome() {
        auto base = fs::temp_directory_path() /
                    ("hermes-migration-test-" + std::to_string(::getpid()) +
                     "-" + std::to_string(++counter_));
        fs::create_directories(base);
        dir_ = base;
        if (const char* old = std::getenv("HERMES_HOME"); old != nullptr) {
            had_old_ = true;
            old_ = old;
        }
        ::setenv("HERMES_HOME", dir_.c_str(), 1);
    }
    ~TempHermesHome() {
        std::error_code ec;
        fs::remove_all(dir_, ec);
        if (had_old_) {
            ::setenv("HERMES_HOME", old_.c_str(), 1);
        } else {
            ::unsetenv("HERMES_HOME");
        }
    }
    const fs::path& path() const { return dir_; }

    void write_config(const std::string& yaml) {
        std::ofstream f(dir_ / "config.yaml");
        f << yaml;
    }

private:
    fs::path dir_;
    bool had_old_ = false;
    std::string old_;
    static inline int counter_ = 0;
};

}  // namespace

// ---------------------------------------------------------------------------
// v1 -> v2: terminal.backend is seeded to "local".
// ---------------------------------------------------------------------------
TEST(MigrationYaml, V1AddsTerminalBackend) {
    nlohmann::json cfg = {
        {"_config_version", 1},
        {"model", "legacy-v1"},
    };
    auto out = hc::migrate_config(cfg);
    EXPECT_EQ(out["terminal"]["backend"].get<std::string>(), "local");
}

TEST(MigrationYaml, V1KeepsExistingTerminalBackend) {
    nlohmann::json cfg = {
        {"_config_version", 1},
        {"terminal", {{"backend", "docker"}}},
    };
    auto out = hc::migrate_config(cfg);
    EXPECT_EQ(out["terminal"]["backend"].get<std::string>(), "docker");
}

// ---------------------------------------------------------------------------
// v2 -> v3: api_key renamed to provider_api_key.
// ---------------------------------------------------------------------------
TEST(MigrationYaml, V2RenamesApiKey) {
    nlohmann::json cfg = {
        {"_config_version", 2},
        {"api_key", "sk-v2"},
    };
    auto out = hc::migrate_config(cfg);
    EXPECT_FALSE(out.contains("api_key"));
    EXPECT_EQ(out["provider_api_key"].get<std::string>(), "sk-v2");
}

// ---------------------------------------------------------------------------
// v3 -> v4: display.skin seeded to "default".
// ---------------------------------------------------------------------------
TEST(MigrationYaml, V3AddsDisplaySkin) {
    nlohmann::json cfg = {{"_config_version", 3}};
    auto out = hc::migrate_config(cfg);
    EXPECT_EQ(out["display"]["skin"].get<std::string>(), "default");
}

// ---------------------------------------------------------------------------
// v4 -> v5: web.backend="exa", tts.provider="edge".
// ---------------------------------------------------------------------------
TEST(MigrationYaml, V4AddsWebAndTts) {
    nlohmann::json cfg = {{"_config_version", 4}};
    auto out = hc::migrate_config(cfg);
    EXPECT_EQ(out["web"]["backend"].get<std::string>(), "exa");
    EXPECT_EQ(out["tts"]["provider"].get<std::string>(), "edge");
}

// ---------------------------------------------------------------------------
// v5 -> v6: security + logging sections seeded.
// ---------------------------------------------------------------------------
TEST(MigrationYaml, V5AddsSecurityAndLogging) {
    nlohmann::json cfg = {{"_config_version", 5}};
    auto out = hc::migrate_config(cfg);

    ASSERT_TRUE(out.contains("security"));
    EXPECT_TRUE(out["security"]["redact_secrets"].get<bool>());
    EXPECT_TRUE(out["security"]["tirith_enabled"].get<bool>());
    EXPECT_EQ(out["security"]["tirith_path"].get<std::string>(), "tirith");
    EXPECT_EQ(out["security"]["tirith_timeout"].get<int>(), 5);
    EXPECT_TRUE(out["security"]["tirith_fail_open"].get<bool>());

    ASSERT_TRUE(out.contains("logging"));
    EXPECT_EQ(out["logging"]["level"].get<std::string>(), "INFO");
    EXPECT_EQ(out["logging"]["max_size_mb"].get<int>(), 5);
    EXPECT_EQ(out["logging"]["backup_count"].get<int>(), 3);

    EXPECT_EQ(out["_config_version"].get<int>(), hc::kCurrentConfigVersion);
}

TEST(MigrationYaml, V5PreservesExistingSecurityOverrides) {
    nlohmann::json cfg = {
        {"_config_version", 5},
        {"security", {{"redact_secrets", false}, {"tirith_timeout", 30}}},
    };
    auto out = hc::migrate_config(cfg);
    EXPECT_FALSE(out["security"]["redact_secrets"].get<bool>());
    EXPECT_EQ(out["security"]["tirith_timeout"].get<int>(), 30);
    // Untouched defaults still filled in.
    EXPECT_EQ(out["security"]["tirith_path"].get<std::string>(), "tirith");
}

// ---------------------------------------------------------------------------
// Full-walk: a synthetic v1 YAML file on disk ends up at kCurrentConfigVersion
// with every intermediate migration applied.
// ---------------------------------------------------------------------------
TEST(MigrationYaml, V1YamlWalksToCurrent) {
    TempHermesHome home;
    home.write_config(
        "_config_version: 1\n"
        "model: legacy\n"
        "api_key: sk-legacy\n");

    // load_cli_config only merges + env-expands; run migrate_config to walk
    // the version chain (matches the real setup-wizard code path).
    auto cfg = hc::migrate_config(hc::load_cli_config());

    // Walked all the way forward.
    EXPECT_EQ(cfg["_config_version"].get<int>(), hc::kCurrentConfigVersion);

    // v1->v2
    EXPECT_EQ(cfg["terminal"]["backend"].get<std::string>(), "local");

    // v2->v3 (note: load_cli_config merges DEFAULT_CONFIG first which does
    // not contain api_key — the overlay's api_key survives, then the
    // migration renames it).
    EXPECT_FALSE(cfg.contains("api_key"));
    EXPECT_EQ(cfg["provider_api_key"].get<std::string>(), "sk-legacy");

    // v3->v4
    EXPECT_EQ(cfg["display"]["skin"].get<std::string>(), "default");

    // v4->v5
    EXPECT_EQ(cfg["web"]["backend"].get<std::string>(), "exa");
    EXPECT_EQ(cfg["tts"]["provider"].get<std::string>(), "edge");

    // v5->v6
    EXPECT_TRUE(cfg.contains("security"));
    EXPECT_TRUE(cfg.contains("logging"));

    // Original model preserved through the whole chain.
    EXPECT_EQ(cfg["model"].get<std::string>(), "legacy");
}

TEST(MigrationYaml, V3YamlSkipsEarlyStepsAndWalksForward) {
    TempHermesHome home;
    home.write_config(
        "_config_version: 3\n"
        "terminal:\n"
        "  backend: docker\n"
        "display:\n"
        "  skin: ocean\n"
        "provider_api_key: sk-kept\n");

    auto cfg = hc::migrate_config(hc::load_cli_config());
    EXPECT_EQ(cfg["_config_version"].get<int>(), hc::kCurrentConfigVersion);
    EXPECT_EQ(cfg["terminal"]["backend"].get<std::string>(), "docker");
    EXPECT_EQ(cfg["display"]["skin"].get<std::string>(), "ocean");
    EXPECT_EQ(cfg["provider_api_key"].get<std::string>(), "sk-kept");
    EXPECT_EQ(cfg["web"]["backend"].get<std::string>(), "exa");
    EXPECT_TRUE(cfg.contains("security"));
}

TEST(MigrationYaml, FutureVersionIsNotDowngraded) {
    nlohmann::json cfg = {{"_config_version", 99}};
    auto out = hc::migrate_config(cfg);
    EXPECT_EQ(out["_config_version"].get<int>(), 99);
    // No seeded sections — migration is a no-op for future versions.
    EXPECT_FALSE(out.contains("security"));
}

// ---------------------------------------------------------------------------
// v6 -> v14: version-only bumps for v6/v7/v8/v9/v10/v11 + the v11→v12 and
// v13→v14 schema changes applied end-to-end.
// ---------------------------------------------------------------------------
TEST(MigrationYaml, V6YamlWalksToCurrentWithLegacyCustomProviders) {
    TempHermesHome home;
    home.write_config(
        "_config_version: 6\n"
        "custom_providers:\n"
        "  - name: Legacy Host\n"
        "    base_url: https://legacy.example.com/v1\n"
        "    api_key: sk-legacy\n"
        "    model: foo\n"
        "stt:\n"
        "  provider: local\n"
        "  model: base\n");

    auto cfg = hc::migrate_config(hc::load_cli_config());
    EXPECT_EQ(cfg["_config_version"].get<int>(), hc::kCurrentConfigVersion);
    EXPECT_EQ(hc::kCurrentConfigVersion, 14);

    // v11 -> v12: custom_providers migrated into providers dict.
    EXPECT_FALSE(cfg.contains("custom_providers"));
    ASSERT_TRUE(cfg["providers"].contains("legacy-host"));
    EXPECT_EQ(cfg["providers"]["legacy-host"]["api"].get<std::string>(),
              "https://legacy.example.com/v1");
    EXPECT_EQ(cfg["providers"]["legacy-host"]["api_key"].get<std::string>(),
              "sk-legacy");
    EXPECT_EQ(cfg["providers"]["legacy-host"]["default_model"].get<std::string>(),
              "foo");

    // v13 -> v14: flat stt.model relocated under stt.local.
    EXPECT_FALSE(cfg["stt"].contains("model"));
    EXPECT_EQ(cfg["stt"]["local"]["model"].get<std::string>(), "base");
}

TEST(MigrationYaml, V8YamlStampsForwardWithoutEnvRewrite) {
    // The Python v8→v9 migration clears ANTHROPIC_TOKEN from .env.  The
    // C++ port intentionally does not touch .env (it's owned by the setup
    // wizard); we only advance the version stamp.
    TempHermesHome home;
    home.write_config(
        "_config_version: 8\n"
        "model: gpt-4o\n");
    auto cfg = hc::migrate_config(hc::load_cli_config());
    EXPECT_EQ(cfg["_config_version"].get<int>(), hc::kCurrentConfigVersion);
    EXPECT_EQ(cfg["model"].get<std::string>(), "gpt-4o");
}
