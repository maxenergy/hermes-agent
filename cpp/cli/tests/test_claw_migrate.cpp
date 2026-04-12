// Tests for the OpenClaw migration tool.
#include <gtest/gtest.h>

#include "hermes/cli/claw_migrate.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;
using namespace hermes::cli::claw;

namespace {

fs::path make_tmpdir(const std::string& tag) {
    auto base = fs::temp_directory_path() /
                ("hermes_claw_" + tag + "_" +
                 std::to_string(::getpid()) + "_" +
                 std::to_string(std::rand()));
    fs::create_directories(base);
    return base;
}

void write_file(const fs::path& p, const std::string& content) {
    fs::create_directories(p.parent_path());
    std::ofstream(p) << content;
}

std::string read_file(const fs::path& p) {
    std::ifstream in(p);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

struct Tmp {
    fs::path openclaw;
    fs::path hermes;
    Tmp() {
        openclaw = make_tmpdir("oc");
        hermes = make_tmpdir("hh");
    }
    ~Tmp() {
        std::error_code ec;
        fs::remove_all(openclaw, ec);
        fs::remove_all(hermes, ec);
    }
    MigrateOptions opts() const {
        MigrateOptions o;
        o.openclaw_dir = openclaw;
        o.hermes_home_override = hermes;
        return o;
    }
};

}  // namespace

TEST(ClawMigrate, DryRunDoesNotWrite) {
    Tmp t;
    write_file(t.openclaw / "SOUL.md", "identity prompt");
    auto opts = t.opts();
    opts.dry_run = true;

    auto r = migrate(opts);
    EXPECT_FALSE(fs::exists(t.hermes / "SOUL.md"));
    EXPECT_GE(r.item_count, 1);
}

TEST(ClawMigrate, SoulRoundTrip) {
    Tmp t;
    write_file(t.openclaw / "SOUL.md", "hello soul");
    auto r = migrate(t.opts());
    ASSERT_TRUE(fs::exists(t.hermes / "SOUL.md"));
    EXPECT_EQ(read_file(t.hermes / "SOUL.md"), "hello soul");
    EXPECT_GE(r.item_count, 1);
}

TEST(ClawMigrate, MemoriesAppendNotReplace) {
    Tmp t;
    write_file(t.openclaw / "MEMORY.md", "new entry");
    write_file(t.hermes / "memories" / "MEMORY.md", "old entry");

    migrate(t.opts());
    auto content = read_file(t.hermes / "memories" / "MEMORY.md");
    EXPECT_NE(content.find("old entry"), std::string::npos);
    EXPECT_NE(content.find("new entry"), std::string::npos);
}

TEST(ClawMigrate, SkillsCopiedToOpenclawImports) {
    Tmp t;
    write_file(t.openclaw / "skills" / "my-skill" / "SKILL.md",
               "# my skill");
    migrate(t.opts());
    EXPECT_TRUE(fs::exists(t.hermes / "skills" / "openclaw-imports" /
                           "my-skill" / "SKILL.md"));
}

TEST(ClawMigrate, ApiKeysAllowlistFilters) {
    Tmp t;
    write_file(t.openclaw / ".env",
               "OPENAI_API_KEY=sk-keep\n"
               "RANDOM_SECRET=drop_me\n"
               "TELEGRAM_BOT_TOKEN=abc123\n");
    migrate(t.opts());
    auto content = read_file(t.hermes / ".env");
    EXPECT_NE(content.find("OPENAI_API_KEY"), std::string::npos);
    EXPECT_NE(content.find("TELEGRAM_BOT_TOKEN"), std::string::npos);
    EXPECT_EQ(content.find("RANDOM_SECRET"), std::string::npos);
}

TEST(ClawMigrate, OverwriteFlagRespected) {
    Tmp t;
    write_file(t.openclaw / "SOUL.md", "new");
    write_file(t.hermes / "SOUL.md", "existing");

    auto opts = t.opts();
    migrate(opts);
    EXPECT_EQ(read_file(t.hermes / "SOUL.md"), "existing");

    opts.overwrite = true;
    migrate(opts);
    EXPECT_EQ(read_file(t.hermes / "SOUL.md"), "new");
}

TEST(ClawMigrate, PresetNoSecretsExcludesApiKeys) {
    Tmp t;
    write_file(t.openclaw / ".env", "OPENAI_API_KEY=sk-xyz\n");
    auto opts = t.opts();
    opts.preset = "no-secrets";
    migrate(opts);
    EXPECT_FALSE(fs::exists(t.hermes / ".env"));
}

TEST(ClawMigrate, WorkspaceTargetCopiesAgentsMd) {
    Tmp t;
    write_file(t.openclaw / "AGENTS.md", "# agents");
    auto ws = make_tmpdir("ws");
    auto opts = t.opts();
    opts.workspace_target = ws;
    migrate(opts);
    EXPECT_TRUE(fs::exists(ws / "AGENTS.md"));
    fs::remove_all(ws);
}

TEST(ClawMigrate, ApiKeyAllowlistContainsExpectedEntries) {
    const auto& k = api_key_allowlist();
    EXPECT_NE(std::find(k.begin(), k.end(), "OPENAI_API_KEY"), k.end());
    EXPECT_NE(std::find(k.begin(), k.end(), "ANTHROPIC_API_KEY"), k.end());
    EXPECT_NE(std::find(k.begin(), k.end(), "TELEGRAM_BOT_TOKEN"), k.end());
    EXPECT_NE(std::find(k.begin(), k.end(), "ELEVENLABS_API_KEY"), k.end());
    EXPECT_NE(std::find(k.begin(), k.end(), "OPENROUTER_API_KEY"), k.end());
}
