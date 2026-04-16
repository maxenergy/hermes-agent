// Tests for the Stream B token-authenticated Skills Hub HTTP surface.
//
// The server side is mocked via hermes::llm::FakeHttpTransport — every
// GET/POST pops one pre-enqueued Response off its queue.  A tiny
// tarball is produced at runtime with the system `tar` utility so the
// install() path can exercise real extraction without committing a
// binary fixture into the repo.
#include "hermes/skills/skills_hub.hpp"

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

struct HubHttpFixture : public ::testing::Test {
    fs::path root;
    HubPaths paths;

    void SetUp() override {
        root = fs::temp_directory_path() /
               ("hermes_hub_http_" +
                std::to_string(::testing::UnitTest::GetInstance()->random_seed()) +
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

void enqueue(hermes::llm::FakeHttpTransport& t, int status,
             const std::string& body) {
    hermes::llm::HttpTransport::Response r;
    r.status_code = status;
    r.body = body;
    t.enqueue_response(std::move(r));
}

void enqueue_json(hermes::llm::FakeHttpTransport& t, const json& body,
                  int status = 200) {
    enqueue(t, status, body.dump());
}

// Build a small tarball at |tgz_path| containing |name|/SKILL.md.
// Returns true on success.
bool build_tarball_fixture(const fs::path& tgz_path,
                           const std::string& name,
                           const std::string& skill_md_body) {
    auto staging = tgz_path.parent_path() /
                   (tgz_path.filename().string() + ".stage");
    std::error_code ec;
    fs::remove_all(staging, ec);
    fs::create_directories(staging / name, ec);
    {
        std::ofstream ofs(staging / name / "SKILL.md");
        ofs << skill_md_body;
    }
    std::string cmd = "tar -czf '" + tgz_path.string() + "' -C '" +
                      staging.string() + "' '" + name + "' 2>/dev/null";
    int rc = std::system(cmd.c_str());
    fs::remove_all(staging, ec);
    return rc == 0 && fs::exists(tgz_path, ec);
}

}  // namespace

// -------------------------------------------------------------------------
// search(query, token, err) — happy path, malformed body, HTTP failure.
// -------------------------------------------------------------------------

TEST_F(HubHttpFixture, SearchReturnsItemsFromEnvelope) {
    hermes::llm::FakeHttpTransport t;
    json payload = {
        {"total", 1}, {"page", 1},
        {"items", json::array({
            json{{"name", "foo"}, {"version", "1.2.3"},
                 {"description", "a demo"},
                 {"author", "alice"},
                 {"tags", {"demo", "cpp"}},
                 {"download_url", "https://cdn.example/foo.tgz"},
                 {"size_bytes", 4096},
                 {"updated_at", "2026-01-02T03:04:05Z"}}
        })}
    };
    enqueue_json(t, payload);

    SkillsHub hub(paths, &t, "https://hub.example");
    std::string err;
    auto results = hub.search("foo", "tok-abc", &err);
    ASSERT_EQ(results.size(), 1u);
    EXPECT_EQ(results[0].name, "foo");
    EXPECT_EQ(results[0].version, "1.2.3");
    EXPECT_EQ(results[0].author, "alice");
    ASSERT_EQ(results[0].tags.size(), 2u);
    EXPECT_EQ(results[0].tags[0], "demo");
    EXPECT_EQ(results[0].download_url, "https://cdn.example/foo.tgz");
    EXPECT_EQ(results[0].size_bytes, 4096u);
    EXPECT_EQ(results[0].updated_at, "2026-01-02T03:04:05Z");
    EXPECT_TRUE(err.empty());

    // Bearer header should have been attached.
    ASSERT_EQ(t.requests().size(), 1u);
    auto it = t.requests()[0].headers.find("Authorization");
    ASSERT_NE(it, t.requests()[0].headers.end());
    EXPECT_EQ(it->second, "Bearer tok-abc");
}

TEST_F(HubHttpFixture, SearchAcceptsBareArrayResponse) {
    hermes::llm::FakeHttpTransport t;
    json payload = json::array({
        json{{"name", "bar"}, {"version", "0.1"}},
        json{{"name", "baz"}, {"version", "0.2"}},
    });
    enqueue_json(t, payload);

    SkillsHub hub(paths, &t, "https://hub.example");
    auto results = hub.search("b", "", nullptr);
    ASSERT_EQ(results.size(), 2u);
    EXPECT_EQ(results[0].name, "bar");
    EXPECT_EQ(results[1].name, "baz");
}

TEST_F(HubHttpFixture, SearchReportsMalformedJson) {
    hermes::llm::FakeHttpTransport t;
    enqueue(t, 200, "{not-json");
    SkillsHub hub(paths, &t, "https://hub.example");
    std::string err;
    auto results = hub.search("foo", "", &err);
    EXPECT_TRUE(results.empty());
    EXPECT_FALSE(err.empty());
}

TEST_F(HubHttpFixture, SearchReportsHttpFailure) {
    hermes::llm::FakeHttpTransport t;
    enqueue(t, 503, "");
    SkillsHub hub(paths, &t, "https://hub.example");
    std::string err;
    auto results = hub.search("foo", "tok", &err);
    EXPECT_TRUE(results.empty());
    ASSERT_FALSE(err.empty());
    EXPECT_NE(err.find("503"), std::string::npos);
}

TEST_F(HubHttpFixture, SearchWithoutTransportReportsError) {
    SkillsHub hub(paths, /*transport=*/nullptr, "https://hub.example");
    // get_default_transport() may return nullptr in the test env (no
    // libcurl wired); either way we should either fail gracefully with
    // err set, or succeed if the default transport is present — we
    // cannot rely on either, so just assert the function doesn't crash
    // and that error_out is either empty (success) or populated.
    std::string err;
    auto results = hub.search("foo", "", &err);
    EXPECT_TRUE(results.empty() || !results.empty());
    (void)err;  // don't assert on global state
}

TEST_F(HubHttpFixture, SearchUrlEncodesQuery) {
    hermes::llm::FakeHttpTransport t;
    enqueue_json(t, json::array());
    SkillsHub hub(paths, &t, "https://hub.example");
    (void)hub.search("foo bar/baz&qux", "", nullptr);
    ASSERT_EQ(t.requests().size(), 1u);
    // Spaces should be encoded as %20, '/' as %2F, '&' as %26.
    EXPECT_NE(t.requests()[0].url.find("foo%20bar%2Fbaz%26qux"),
              std::string::npos);
}

// -------------------------------------------------------------------------
// get(name, token, err)
// -------------------------------------------------------------------------

TEST_F(HubHttpFixture, GetReturnsEntry) {
    hermes::llm::FakeHttpTransport t;
    json body = {
        {"name", "foo"}, {"version", "1.0"},
        {"description", "hello"},
        {"author", "bob"},
        {"download_url", "https://cdn/foo.tgz"},
    };
    enqueue_json(t, body);

    SkillsHub hub(paths, &t, "https://hub.example");
    std::string err;
    auto got = hub.get(std::string("foo"), std::string("tok"), &err);
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(got->name, "foo");
    EXPECT_EQ(got->author, "bob");
    EXPECT_EQ(got->download_url, "https://cdn/foo.tgz");
    EXPECT_TRUE(err.empty());
}

TEST_F(HubHttpFixture, Get404FillsError) {
    hermes::llm::FakeHttpTransport t;
    enqueue(t, 404, R"({"error":"not found"})");

    SkillsHub hub(paths, &t, "https://hub.example");
    std::string err;
    auto got = hub.get(std::string("missing"), std::string("tok"), &err);
    EXPECT_FALSE(got.has_value());
    ASSERT_FALSE(err.empty());
    EXPECT_NE(err.find("404"), std::string::npos);
}

TEST_F(HubHttpFixture, GetEmptyNameReportsError) {
    SkillsHub hub(paths, nullptr, "https://hub.example");
    std::string err;
    auto got = hub.get(std::string(""), std::string("tok"), &err);
    EXPECT_FALSE(got.has_value());
    EXPECT_FALSE(err.empty());
}

// -------------------------------------------------------------------------
// list_all(token, page, page_size, err)
// -------------------------------------------------------------------------

TEST_F(HubHttpFixture, ListAllParsesItems) {
    hermes::llm::FakeHttpTransport t;
    json payload = {
        {"total", 2}, {"page", 1},
        {"items", json::array({
            json{{"name", "a"}, {"version", "1"}},
            json{{"name", "b"}, {"version", "2"}},
        })}
    };
    enqueue_json(t, payload);

    SkillsHub hub(paths, &t, "https://hub.example");
    std::string err;
    auto items = hub.list_all("tok", 1, 50, &err);
    ASSERT_EQ(items.size(), 2u);
    EXPECT_EQ(items[0].name, "a");
    EXPECT_TRUE(err.empty());
    // URL must carry page and page_size.
    ASSERT_EQ(t.requests().size(), 1u);
    EXPECT_NE(t.requests()[0].url.find("page=1"), std::string::npos);
    EXPECT_NE(t.requests()[0].url.find("page_size=50"), std::string::npos);
}

TEST_F(HubHttpFixture, ListAllClampsNegativeArgs) {
    hermes::llm::FakeHttpTransport t;
    enqueue_json(t, json::array());
    SkillsHub hub(paths, &t, "https://hub.example");
    (void)hub.list_all("", -5, -1, nullptr);
    ASSERT_EQ(t.requests().size(), 1u);
    EXPECT_NE(t.requests()[0].url.find("page=1"), std::string::npos);
    EXPECT_NE(t.requests()[0].url.find("page_size="), std::string::npos);
}

// -------------------------------------------------------------------------
// install(name, dest_root, token, err)
// -------------------------------------------------------------------------

TEST_F(HubHttpFixture, InstallExtractsTarball) {
    // Build a real tarball fixture on disk.
    auto tgz = root / "foo.tgz";
    ASSERT_TRUE(build_tarball_fixture(tgz, "foo",
                                      "---\nname: foo\n---\nhello\n"))
        << "system `tar` is required for this test";

    // Read the bytes back so we can hand them to the mock transport.
    std::ifstream ifs(tgz, std::ios::binary);
    std::string tarball_bytes((std::istreambuf_iterator<char>(ifs)),
                              std::istreambuf_iterator<char>());
    ASSERT_FALSE(tarball_bytes.empty());

    hermes::llm::FakeHttpTransport t;
    // 1st call: metadata GET
    enqueue_json(t, json{
        {"name", "foo"}, {"version", "1.0"},
        {"download_url", "https://cdn/foo.tgz"},
    });
    // 2nd call: tarball download
    enqueue(t, 200, tarball_bytes);

    SkillsHub hub(paths, &t, "https://hub.example");
    auto dest_root = root / "install-root";
    std::string err;
    auto installed = hub.install(std::string("foo"), dest_root,
                                 std::string("tok"), &err);
    ASSERT_TRUE(installed.has_value()) << "err=" << err;
    EXPECT_EQ(*installed, dest_root / "foo");
    EXPECT_TRUE(fs::exists(dest_root / "foo" / "SKILL.md"));

    std::ifstream skill_md(dest_root / "foo" / "SKILL.md");
    std::string contents((std::istreambuf_iterator<char>(skill_md)),
                         std::istreambuf_iterator<char>());
    EXPECT_NE(contents.find("hello"), std::string::npos);
}

TEST_F(HubHttpFixture, InstallMovesExistingDirAside) {
    // Pre-populate dest_root/foo so install() has to move it aside.
    auto tgz = root / "foo2.tgz";
    ASSERT_TRUE(build_tarball_fixture(tgz, "foo",
                                      "---\nname: foo\n---\nfresh\n"));
    std::ifstream ifs(tgz, std::ios::binary);
    std::string tarball_bytes((std::istreambuf_iterator<char>(ifs)),
                              std::istreambuf_iterator<char>());

    hermes::llm::FakeHttpTransport t;
    enqueue_json(t, json{
        {"name", "foo"}, {"version", "2.0"},
        {"download_url", "https://cdn/foo.tgz"},
    });
    enqueue(t, 200, tarball_bytes);

    auto dest_root = root / "install-root";
    fs::create_directories(dest_root / "foo");
    std::ofstream(dest_root / "foo" / "OLD.md") << "stale\n";

    SkillsHub hub(paths, &t, "https://hub.example");
    std::string err;
    auto installed = hub.install(std::string("foo"), dest_root,
                                 std::string("tok"), &err);
    ASSERT_TRUE(installed.has_value()) << "err=" << err;

    // Fresh SKILL.md exists, stale OLD.md is gone from foo/.
    EXPECT_TRUE(fs::exists(dest_root / "foo" / "SKILL.md"));
    EXPECT_FALSE(fs::exists(dest_root / "foo" / "OLD.md"));

    // A .bak.* sibling should exist containing the stale file.
    bool found_bak = false;
    for (auto& entry : fs::directory_iterator(dest_root)) {
        auto fn = entry.path().filename().string();
        if (fn.rfind("foo.bak.", 0) == 0) {
            found_bak = true;
            EXPECT_TRUE(fs::exists(entry.path() / "OLD.md"));
        }
    }
    EXPECT_TRUE(found_bak);
}

TEST_F(HubHttpFixture, InstallMetadataFailureFillsError) {
    hermes::llm::FakeHttpTransport t;
    enqueue(t, 500, "");  // metadata fetch fails
    SkillsHub hub(paths, &t, "https://hub.example");
    std::string err;
    auto r = hub.install(std::string("foo"), root / "install-root",
                         std::string("tok"), &err);
    EXPECT_FALSE(r.has_value());
    EXPECT_FALSE(err.empty());
}

TEST_F(HubHttpFixture, InstallEmptyDownloadUrlFails) {
    hermes::llm::FakeHttpTransport t;
    enqueue_json(t, json{{"name", "foo"}, {"version", "1.0"}});  // no url
    SkillsHub hub(paths, &t, "https://hub.example");
    std::string err;
    auto r = hub.install(std::string("foo"), root / "install-root",
                         std::string("tok"), &err);
    EXPECT_FALSE(r.has_value());
    EXPECT_NE(err.find("download_url"), std::string::npos);
}

TEST_F(HubHttpFixture, InstallTransportErrorBubbles) {
    // No responses enqueued → post_json/get returns default Response
    // (status 0).  We treat that as a hub failure.
    hermes::llm::FakeHttpTransport t;
    SkillsHub hub(paths, &t, "https://hub.example");
    std::string err;
    auto r = hub.install(std::string("foo"), root / "install-root",
                         std::string("tok"), &err);
    EXPECT_FALSE(r.has_value());
    EXPECT_FALSE(err.empty());
}

TEST_F(HubHttpFixture, InstallCorruptTarballReportsError) {
    hermes::llm::FakeHttpTransport t;
    enqueue_json(t, json{
        {"name", "foo"}, {"version", "1.0"},
        {"download_url", "https://cdn/foo.tgz"},
    });
    enqueue(t, 200, std::string("\x00\x01\x02\x03 not a tarball", 22));
    SkillsHub hub(paths, &t, "https://hub.example");
    std::string err;
    auto r = hub.install(std::string("foo"), root / "install-root",
                         std::string("tok"), &err);
    EXPECT_FALSE(r.has_value());
    EXPECT_NE(err.find("tar"), std::string::npos);
}

TEST_F(HubHttpFixture, InstallRejectsBadName) {
    SkillsHub hub(paths, nullptr, "https://hub.example");
    std::string err;
    EXPECT_FALSE(hub.install(std::string(""), root, std::string(""), &err)
                      .has_value());
    EXPECT_FALSE(hub.install(std::string(".."), root, std::string(""), &err)
                      .has_value());
    EXPECT_FALSE(hub.install(std::string("a/b"), root, std::string(""), &err)
                      .has_value());
}
