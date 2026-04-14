// Tests for the Skills Hub port.
//
// The original smoke tests (which just asserted the stub returned empty
// results) are preserved below; the rest of this file exercises the
// extended surface — paths, cache, lock file, tap store, version
// parsing, dependency resolution, content hashing, scan, audit log,
// install rollback, and the end-to-end install/uninstall/update loop
// via an injected FakeHttpTransport.
#include "hermes/skills/skills_hub.hpp"

#include "hermes/core/atomic_io.hpp"
#include "hermes/llm/llm_client.hpp"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;
using namespace hermes::skills;
using json = nlohmann::json;

namespace {

// Fixture that sets up an isolated skills hub root + fake transport.
struct HubFixture : public ::testing::Test {
    fs::path root;
    HubPaths paths;

    void SetUp() override {
        root = fs::temp_directory_path() /
               ("hermes_hub_test_" + std::to_string(::testing::UnitTest::GetInstance()->random_seed()) +
                "_" + std::to_string(reinterpret_cast<std::uintptr_t>(this)));
        fs::remove_all(root);
        fs::create_directories(root);
        paths = HubPaths::for_root(root);
        ensure_hub_paths(paths);
    }

    void TearDown() override {
        std::error_code ec;
        fs::remove_all(root, ec);
    }
};

// Helper: register a JSON response on the fake transport.
void enqueue_json(hermes::llm::FakeHttpTransport& t, const json& body,
                  int status = 200) {
    hermes::llm::HttpTransport::Response r;
    r.status_code = status;
    r.body = body.dump();
    t.enqueue_response(std::move(r));
}

void enqueue_status(hermes::llm::FakeHttpTransport& t, int status) {
    hermes::llm::HttpTransport::Response r;
    r.status_code = status;
    r.body = "";
    t.enqueue_response(std::move(r));
}

}  // namespace

// -------------------------------------------------------------------------
// Legacy smoke tests — still valid after the extension.
// -------------------------------------------------------------------------

namespace {
// Produce an isolated hub rooted under /tmp so these legacy "smoke" tests
// don't scribble in the developer's real ~/.hermes/skills.
SkillsHub make_isolated_hub() {
    static int ctr = 0;
    auto root = fs::temp_directory_path() /
                ("hermes_hub_smoke_" + std::to_string(++ctr) + "_" +
                 std::to_string(reinterpret_cast<std::uintptr_t>(&ctr)));
    fs::remove_all(root);
    fs::create_directories(root);
    auto paths = HubPaths::for_root(root);
    ensure_hub_paths(paths);
    return SkillsHub(paths, /*transport=*/nullptr, "https://test.hub");
}
}  // namespace

TEST(SkillsHubTest, SearchReturnsEmptyWithoutTransport) {
    auto hub = make_isolated_hub();
    auto results = hub.search("anything");
    EXPECT_TRUE(results.empty());
}

TEST(SkillsHubTest, InstallReturnsFalseWithoutTransport) {
    auto hub = make_isolated_hub();
    EXPECT_FALSE(hub.install("some-skill"));
}

TEST(SkillsHubTest, GetReturnsNulloptWithoutTransport) {
    auto hub = make_isolated_hub();
    EXPECT_FALSE(hub.get("whatever").has_value());
}

TEST(SkillsHubTest, UninstallReturnsFalseForNonexistent) {
    auto hub = make_isolated_hub();
    EXPECT_FALSE(hub.uninstall("nonexistent-skill-xyz"));
}

TEST(SkillsHubTest, UpdateReturnsFalseWithoutTransport) {
    auto hub = make_isolated_hub();
    EXPECT_FALSE(hub.update("some-skill"));
}

// -------------------------------------------------------------------------
// TrustLevel ↔ string
// -------------------------------------------------------------------------

TEST(SkillsHubTrust, Roundtrip) {
    for (auto lvl : {TrustLevel::Builtin, TrustLevel::Trusted,
                     TrustLevel::Community}) {
        EXPECT_EQ(trust_level_from_string(to_string(lvl)), lvl);
    }
    EXPECT_EQ(trust_level_from_string("bogus"), TrustLevel::Community);
}

// -------------------------------------------------------------------------
// HubPaths
// -------------------------------------------------------------------------

TEST_F(HubFixture, PathsLayout) {
    EXPECT_EQ(paths.hub_dir, paths.skills_dir / ".hub");
    EXPECT_EQ(paths.lock_file, paths.hub_dir / "lock.json");
    EXPECT_TRUE(fs::exists(paths.hub_dir));
    EXPECT_TRUE(fs::exists(paths.quarantine_dir));
    EXPECT_TRUE(fs::exists(paths.index_cache_dir));
}

// -------------------------------------------------------------------------
// IndexCache
// -------------------------------------------------------------------------

TEST_F(HubFixture, IndexCachePutGet) {
    IndexCache c(paths.index_cache_dir);
    EXPECT_FALSE(c.get("foo").has_value());
    c.put("foo", R"({"a":1})");
    auto got = c.get("foo");
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(*got, R"({"a":1})");
}

TEST_F(HubFixture, IndexCacheClear) {
    IndexCache c(paths.index_cache_dir);
    c.put("a", "1");
    c.put("b", "2");
    c.clear();
    EXPECT_FALSE(c.get("a").has_value());
    EXPECT_FALSE(c.get("b").has_value());
}

TEST_F(HubFixture, IndexCachePruneExpired) {
    // Use a 0-second TTL so every entry is immediately expired.
    IndexCache c(paths.index_cache_dir, std::chrono::seconds(0));
    c.put("a", "1");
    c.put("b", "2");
    auto pruned = c.prune_expired();
    EXPECT_GE(pruned, 2u);
}

// -------------------------------------------------------------------------
// HubLockFile
// -------------------------------------------------------------------------

TEST_F(HubFixture, LockFileRoundtrip) {
    HubLockFile lf(paths.lock_file);
    lf.load();
    EXPECT_EQ(lf.size(), 0u);

    HubLockEntry e;
    e.name = "demo";
    e.source = "official";
    e.identifier = "anthropics/skills/demo";
    e.version = "1.2.3";
    e.content_hash = "abc123";
    e.trust = TrustLevel::Trusted;
    e.installed_at = std::chrono::system_clock::now();
    lf.upsert(e);
    ASSERT_TRUE(lf.save());

    HubLockFile lf2(paths.lock_file);
    lf2.load();
    EXPECT_EQ(lf2.size(), 1u);
    auto got = lf2.get("demo");
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(got->version, "1.2.3");
    EXPECT_EQ(got->trust, TrustLevel::Trusted);
}

TEST_F(HubFixture, LockFileRemove) {
    HubLockFile lf(paths.lock_file);
    HubLockEntry e; e.name = "x";
    lf.upsert(e);
    EXPECT_TRUE(lf.contains("x"));
    EXPECT_TRUE(lf.remove("x"));
    EXPECT_FALSE(lf.remove("x"));
    EXPECT_FALSE(lf.contains("x"));
}

// -------------------------------------------------------------------------
// TapStore
// -------------------------------------------------------------------------

TEST_F(HubFixture, TapStoreDefaults) {
    TapStore ts(paths.taps_file);
    ts.load();
    EXPECT_FALSE(ts.taps().empty());
    bool found_openai = false;
    for (const auto& t : ts.taps()) {
        if (t.repo == "openai/skills") found_openai = true;
    }
    EXPECT_TRUE(found_openai);
}

TEST_F(HubFixture, TapStoreAddRemove) {
    TapStore ts(paths.taps_file);
    HubTap t;
    t.repo = "me/myrepo";
    t.path = "s/";
    t.trust = TrustLevel::Trusted;
    EXPECT_TRUE(ts.add(t));
    EXPECT_FALSE(ts.add(t));  // duplicate
    EXPECT_TRUE(ts.save());

    TapStore ts2(paths.taps_file);
    ts2.load();
    bool found = false;
    for (const auto& tap : ts2.taps()) {
        if (tap.repo == "me/myrepo" && tap.path == "s/") {
            found = true;
            EXPECT_EQ(tap.trust, TrustLevel::Trusted);
        }
    }
    EXPECT_TRUE(found);
    EXPECT_TRUE(ts2.remove("me/myrepo", "s/"));
    EXPECT_FALSE(ts2.remove("me/myrepo", "s/"));
}

// -------------------------------------------------------------------------
// Spec / version helpers
// -------------------------------------------------------------------------

TEST(SkillsHubVersions, ParseSpec) {
    EXPECT_EQ(parse_spec("foo").name, "foo");
    EXPECT_EQ(parse_spec("foo").version, "");
    EXPECT_EQ(parse_spec("foo@1.2.3").name, "foo");
    EXPECT_EQ(parse_spec("foo@1.2.3").version, "1.2.3");
    EXPECT_EQ(parse_spec(" foo @ 1.0 ").name, "foo");
    EXPECT_EQ(parse_spec(" foo @ 1.0 ").version, "1.0");
}

TEST(SkillsHubVersions, CompareVersions) {
    EXPECT_LT(compare_versions("1.0.0", "1.0.1"), 0);
    EXPECT_GT(compare_versions("2.0", "1.9.9"), 0);
    EXPECT_EQ(compare_versions("1.2.3", "1.2.3"), 0);
    EXPECT_LT(compare_versions("1.2", "1.2.1"), 0);  // missing == 0
    EXPECT_GT(compare_versions("10.0", "9.0"), 0);
    EXPECT_EQ(compare_versions("0", "0.0.0"), 0);
}

TEST(SkillsHubVersions, SatisfiesPin) {
    EXPECT_TRUE(satisfies_pin("1.2.3", ""));
    EXPECT_TRUE(satisfies_pin("1.2.3", "latest"));
    EXPECT_TRUE(satisfies_pin("1.2.3", "*"));
    EXPECT_TRUE(satisfies_pin("1.2.3", "1.2.3"));
    EXPECT_FALSE(satisfies_pin("1.2.3", "1.2.4"));
    EXPECT_TRUE(satisfies_pin("2.0.0", ">=1.0"));
    EXPECT_FALSE(satisfies_pin("0.9.0", ">=1.0"));
    EXPECT_TRUE(satisfies_pin("0.9.0", "<1.0"));
    EXPECT_TRUE(satisfies_pin("1.0.0", "=1.0.0"));
}

// -------------------------------------------------------------------------
// topo_order
// -------------------------------------------------------------------------

TEST(SkillsHubDeps, LinearChain) {
    std::unordered_map<std::string, std::vector<std::string>> g = {
        {"a", {"b"}},
        {"b", {"c"}},
        {"c", {}},
    };
    auto order = topo_order("a", [&](const std::string& n) { return g[n]; });
    ASSERT_EQ(order.size(), 3u);
    EXPECT_EQ(order.front(), "c");
    EXPECT_EQ(order.back(), "a");
}

TEST(SkillsHubDeps, CycleBroken) {
    std::unordered_map<std::string, std::vector<std::string>> g = {
        {"a", {"b"}},
        {"b", {"a"}},
    };
    auto order = topo_order("a", [&](const std::string& n) { return g[n]; });
    // Should still terminate with exactly two unique names.
    std::unordered_set<std::string> seen(order.begin(), order.end());
    EXPECT_EQ(seen.size(), 2u);
}

// -------------------------------------------------------------------------
// sha256 / hash_skill_dir
// -------------------------------------------------------------------------

TEST(SkillsHubHash, KnownVector) {
    EXPECT_EQ(
        sha256_hex(""),
        "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    EXPECT_EQ(
        sha256_hex("abc"),
        "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
}

TEST_F(HubFixture, HashSkillDirStable) {
    auto d = root / "s1";
    fs::create_directories(d);
    {
        std::ofstream(d / "a.txt") << "hello";
        std::ofstream(d / "b.txt") << "world";
    }
    auto h1 = hash_skill_dir(d);
    auto h2 = hash_skill_dir(d);
    EXPECT_FALSE(h1.empty());
    EXPECT_EQ(h1, h2);

    // Same files in a sibling dir -> same hash.
    auto d2 = root / "s2";
    fs::create_directories(d2);
    std::ofstream(d2 / "a.txt") << "hello";
    std::ofstream(d2 / "b.txt") << "world";
    EXPECT_EQ(hash_skill_dir(d2), h1);

    // Modified contents -> different hash.
    std::ofstream(d2 / "b.txt") << "different";
    EXPECT_NE(hash_skill_dir(d2), h1);
}

TEST(SkillsHubHash, MissingDirReturnsEmpty) {
    EXPECT_EQ(hash_skill_dir("/nonexistent/path/xyz"), "");
}

// -------------------------------------------------------------------------
// Full SkillsHub flows with an injected FakeHttpTransport.
// -------------------------------------------------------------------------

TEST_F(HubFixture, SearchHitsCacheOnSecondCall) {
    hermes::llm::FakeHttpTransport t;
    json payload = json::array({
        json{{"name", "foo"}, {"description", "a demo"}, {"version", "1.0"}},
    });
    enqueue_json(t, payload);

    SkillsHub hub(paths, &t, "https://test.hub");
    auto r1 = hub.search("foo");
    ASSERT_EQ(r1.size(), 1u);
    EXPECT_EQ(r1[0].name, "foo");

    // Second call should hit cache — no new requests enqueued.
    auto r2 = hub.search("foo");
    ASSERT_EQ(r2.size(), 1u);
    EXPECT_EQ(t.requests().size(), 1u);
}

TEST_F(HubFixture, SearchHttpFailureReturnsEmpty) {
    hermes::llm::FakeHttpTransport t;
    enqueue_status(t, 500);
    SkillsHub hub(paths, &t, "https://test.hub");
    auto r = hub.search("anything");
    EXPECT_TRUE(r.empty());
}

TEST_F(HubFixture, GetReturnsEntry) {
    hermes::llm::FakeHttpTransport t;
    json body = {
        {"name", "foo"}, {"version", "1.0.0"},
        {"description", "d"}, {"source", "github"},
        {"trust", "trusted"}, {"tags", {"x", "y"}},
        {"dependencies", {"bar@1.0"}}
    };
    enqueue_json(t, body);
    SkillsHub hub(paths, &t, "https://test.hub");
    auto got = hub.get("foo");
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(got->version, "1.0.0");
    EXPECT_EQ(got->trust, TrustLevel::Trusted);
    ASSERT_EQ(got->tags.size(), 2u);
    ASSERT_EQ(got->dependencies.size(), 1u);
    EXPECT_EQ(got->dependencies[0], "bar@1.0");
}

TEST_F(HubFixture, InstallLeafSkill) {
    hermes::llm::FakeHttpTransport t;
    // resolve_install_order → get(foo); install → get(foo) again.  We
    // enqueue two identical responses to cover both fetches (second
    // should hit the cache but safer to enqueue).
    json body = {
        {"name", "foo"}, {"version", "1.0.0"},
        {"description", "d"}, {"source", "official"},
        {"trust", "trusted"}, {"dependencies", json::array()},
    };
    enqueue_json(t, body);
    enqueue_json(t, body);
    enqueue_status(t, 404);  // SKILL.md fetch

    SkillsHub hub(paths, &t, "https://test.hub");
    EXPECT_TRUE(hub.install("foo"));
    EXPECT_TRUE(hub.lock().contains("foo"));
    EXPECT_TRUE(fs::exists(paths.skills_dir / "foo" / "metadata.json"));
}

TEST_F(HubFixture, InstallFailsWhenTransportBroken) {
    // No responses enqueued — every HTTP call returns an empty Response
    // (status 0), which we treat as failure.
    hermes::llm::FakeHttpTransport t;
    SkillsHub hub(paths, &t, "https://test.hub");
    EXPECT_FALSE(hub.install("foo"));
    EXPECT_FALSE(hub.lock().contains("foo"));
}

TEST_F(HubFixture, InstallFailsAndRollsBackOnLaterFetchFailure) {
    // foo has one dependency, bar.  We set up the resolve phase to
    // succeed (both lookups return metadata), then during the install
    // loop the bar fetch *also* succeeds but its name is empty so
    // stage_() rejects it — which should prevent foo from installing.
    hermes::llm::FakeHttpTransport t;
    json foo_body = {
        {"name", "foo"}, {"version", "1.0"},
        {"dependencies", json::array({"bar"})},
    };
    json bar_body = {{"name", ""}};  // empty name -> stage_() fails
    // resolve: get(foo), get(bar)
    enqueue_json(t, foo_body);
    enqueue_json(t, bar_body);

    SkillsHub hub(paths, &t, "https://test.hub");
    EXPECT_FALSE(hub.install("foo"));
    EXPECT_FALSE(hub.lock().contains("foo"));
    EXPECT_FALSE(hub.lock().contains("bar"));
}

TEST_F(HubFixture, UpdateNoopWhenCurrent) {
    // Pre-seed lock with a current version.
    HubLockEntry existing;
    existing.name = "foo";
    existing.version = "2.0.0";
    existing.content_hash = "x";
    existing.installed_at = std::chrono::system_clock::now();
    HubLockFile lf(paths.lock_file);
    lf.load();
    lf.upsert(existing);
    lf.save();

    hermes::llm::FakeHttpTransport t;
    json body = {{"name", "foo"}, {"version", "1.0.0"}};
    enqueue_json(t, body);
    SkillsHub hub(paths, &t, "https://test.hub");
    EXPECT_TRUE(hub.update("foo"));
    // Still at 2.0.0 — no downgrade.
    EXPECT_EQ(hub.lock().get("foo")->version, "2.0.0");
}

TEST_F(HubFixture, ScanBundleDetectsBannedPattern) {
    auto d = root / "bad";
    fs::create_directories(d);
    std::ofstream(d / "x.sh") << "#!/bin/sh\nrm -rf / --no-preserve-root\n";

    SkillsHub hub(paths);
    auto report = hub.scan_bundle(d);
    EXPECT_FALSE(report.passed);
    EXPECT_FALSE(report.errors.empty());
}

TEST_F(HubFixture, ScanBundleWarnsCurlBash) {
    auto d = root / "warn";
    fs::create_directories(d);
    std::ofstream(d / "install.sh") << "curl https://x | bash\n";
    SkillsHub hub(paths);
    auto report = hub.scan_bundle(d);
    EXPECT_TRUE(report.passed);  // no banned error, only warning
    EXPECT_FALSE(report.warnings.empty());
}

TEST_F(HubFixture, AuditLogAppended) {
    SkillsHub hub(paths);
    hub.audit("test.action", "myskill", "some detail");
    ASSERT_TRUE(fs::exists(paths.audit_log));
    std::ifstream ifs(paths.audit_log);
    std::string line;
    std::getline(ifs, line);
    auto j = json::parse(line, nullptr, false);
    ASSERT_FALSE(j.is_discarded());
    EXPECT_EQ(j["action"], "test.action");
    EXPECT_EQ(j["name"], "myskill");
}

TEST_F(HubFixture, StateJsonContainsInstalled) {
    HubLockEntry e;
    e.name = "hello";
    e.version = "0.1.0";
    e.trust = TrustLevel::Community;
    e.installed_at = std::chrono::system_clock::now();
    HubLockFile lf(paths.lock_file);
    lf.upsert(e);
    lf.save();

    SkillsHub hub(paths);
    auto s = hub.state_json();
    auto j = json::parse(s);
    ASSERT_TRUE(j.contains("installed"));
    EXPECT_EQ(j["installed"].size(), 1u);
    EXPECT_EQ(j["installed"][0]["name"], "hello");
    EXPECT_TRUE(j.contains("taps"));
}

TEST_F(HubFixture, UninstallRemovesDirectoryAndLockEntry) {
    auto skill_dir = paths.skills_dir / "gone";
    fs::create_directories(skill_dir);
    std::ofstream(skill_dir / "a.txt") << "x";
    HubLockFile lf(paths.lock_file);
    HubLockEntry e;
    e.name = "gone"; e.version = "1.0";
    lf.upsert(e);
    lf.save();

    SkillsHub hub(paths);
    EXPECT_TRUE(hub.uninstall("gone"));
    EXPECT_FALSE(fs::exists(skill_dir));
    EXPECT_FALSE(hub.lock().contains("gone"));
}

TEST_F(HubFixture, VerifyDetectsDrift) {
    auto skill_dir = paths.skills_dir / "chk";
    fs::create_directories(skill_dir);
    std::ofstream(skill_dir / "x.txt") << "one";
    auto good = hash_skill_dir(skill_dir);

    HubLockFile lf(paths.lock_file);
    HubLockEntry e;
    e.name = "chk";
    e.content_hash = good;
    e.installed_at = std::chrono::system_clock::now();
    lf.upsert(e);
    lf.save();

    SkillsHub hub(paths);
    EXPECT_TRUE(hub.verify("chk"));

    // Corrupt the skill.
    std::ofstream(skill_dir / "x.txt") << "two";
    EXPECT_FALSE(hub.verify("chk"));
}
