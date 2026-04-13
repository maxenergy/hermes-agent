#include "hermes/cli/update_prompt.hpp"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>

#include <gtest/gtest.h>

namespace fs = std::filesystem;
using namespace hermes::cli::update_prompt;

namespace {

fs::path tmp_state_path(const std::string& tag) {
    fs::path dir = fs::temp_directory_path() /
                   ("hermes_update_test_" + tag + "_" +
                    std::to_string(::getpid()));
    fs::create_directories(dir);
    return dir / "update.json";
}

}  // namespace

TEST(UpdatePromptVersion, IsNewerVersionBasic) {
    EXPECT_TRUE(is_newer_version("0.1.0", "0.2.0"));
    EXPECT_TRUE(is_newer_version("v0.1.0", "v0.1.1"));
    EXPECT_FALSE(is_newer_version("0.2.0", "0.1.5"));
    EXPECT_FALSE(is_newer_version("1.0.0", "1.0.0"));
}

TEST(UpdatePromptVersion, PreReleaseSortsBeforeRelease) {
    // 0.2.0-rc1 is older than 0.2.0 release.
    EXPECT_TRUE(is_newer_version("0.2.0-rc1", "0.2.0"));
    EXPECT_FALSE(is_newer_version("0.2.0", "0.2.0-rc1"));
}

TEST(UpdatePromptManifest, ParseLatestJson) {
    auto m = parse_latest_manifest(
        R"({"version":"0.5.0","url":"https://x","notes":"bug fixes"})");
    ASSERT_TRUE(m.has_value());
    EXPECT_EQ(m->version, "0.5.0");
    EXPECT_EQ(m->url, "https://x");
    EXPECT_EQ(m->notes, "bug fixes");
}

TEST(UpdatePromptManifest, ParseAcceptsLatestVersionField) {
    auto m = parse_latest_manifest(R"({"latest_version":"1.2.3"})");
    ASSERT_TRUE(m.has_value());
    EXPECT_EQ(m->version, "1.2.3");
}

TEST(UpdatePromptManifest, ParseRejectsBadJson) {
    EXPECT_FALSE(parse_latest_manifest("not json").has_value());
    EXPECT_FALSE(parse_latest_manifest("{}").has_value());
    EXPECT_FALSE(parse_latest_manifest("[1,2,3]").has_value());
}

TEST(UpdatePromptThrottle, RoundTrip) {
    auto path = tmp_state_path("throttle_rt");
    fs::remove(path);
    ThrottleState s;
    s.last_check =
        std::chrono::system_clock::time_point(std::chrono::seconds(1700000000));
    s.last_seen_version = "0.4.2";
    save_throttle(path, s);
    auto loaded = load_throttle(path);
    EXPECT_TRUE(loaded.exists);
    EXPECT_EQ(loaded.last_seen_version, "0.4.2");
    EXPECT_EQ(std::chrono::duration_cast<std::chrono::seconds>(
                  loaded.last_check.time_since_epoch())
                  .count(),
              1700000000);
    fs::remove(path);
}

TEST(UpdatePromptGate, SkipsOnCiEnvVar) {
    auto path = tmp_state_path("ci");
    fs::remove(path);
    ::setenv("CI", "true", 1);
    UpdateConfig cfg;
    cfg.current_version = "0.1.0";
    cfg.state_path_override = path;
    cfg.fetch = [](const std::string&) -> std::optional<std::string> {
        ADD_FAILURE() << "fetch should not be called when CI=true";
        return std::nullopt;
    };
    auto out = maybe_prompt_update(cfg);
    EXPECT_TRUE(out.skipped_no_tty);
    EXPECT_FALSE(out.prompted);
    ::unsetenv("CI");
    fs::remove(path);
}

TEST(UpdatePromptGate, SkipsWhenNoUpdateCheckFlag) {
    auto path = tmp_state_path("nouc");
    fs::remove(path);
    UpdateConfig cfg;
    cfg.current_version = "0.1.0";
    cfg.state_path_override = path;
    cfg.no_update_check_flag = true;
    cfg.fetch = [](const std::string&) -> std::optional<std::string> {
        ADD_FAILURE() << "fetch should not be called with --no-update-check";
        return std::nullopt;
    };
    auto out = maybe_prompt_update(cfg);
    EXPECT_FALSE(out.prompted);
    EXPECT_NE(out.detail.find("--no-update-check"), std::string::npos);
    fs::remove(path);
}

TEST(UpdatePromptGate, ThrottleSkipsRecentCheck) {
    auto path = tmp_state_path("throttle");
    fs::remove(path);
    auto now = std::chrono::system_clock::now();
    ThrottleState s;
    s.last_check = now - std::chrono::hours(2);
    s.last_seen_version = "0.1.0";
    save_throttle(path, s);

    UpdateConfig cfg;
    cfg.current_version = "0.1.0";
    cfg.state_path_override = path;
    cfg.force = true;        // bypass TTY/CI; we still want throttle gate
    cfg.now = now;
    bool fetched = false;
    cfg.fetch = [&](const std::string&) -> std::optional<std::string> {
        fetched = true;
        return std::string(R"({"version":"99.0.0"})");
    };
    auto out = maybe_prompt_update(cfg);
    EXPECT_TRUE(out.throttled);
    EXPECT_FALSE(fetched);
    fs::remove(path);
}

TEST(UpdatePromptFlow, PromptsAndAcceptsYes) {
    auto path = tmp_state_path("yes");
    fs::remove(path);
    UpdateConfig cfg;
    cfg.current_version = "0.1.0";
    cfg.state_path_override = path;
    cfg.force = true;
    cfg.fetch = [](const std::string&) -> std::optional<std::string> {
        return std::string(R"({"version":"0.2.0","url":"https://x"})");
    };
    std::string captured_prompt;
    cfg.write_line = [&](std::string_view s) { captured_prompt = s; };
    cfg.read_line = []() -> std::optional<std::string> {
        return std::string("y");
    };
    auto out = maybe_prompt_update(cfg);
    EXPECT_TRUE(out.prompted);
    EXPECT_TRUE(out.user_accepted);
    EXPECT_NE(captured_prompt.find("v0.1.0"), std::string::npos);
    EXPECT_NE(captured_prompt.find("v0.2.0"), std::string::npos);
    // Throttle file written so a follow-up call would skip.
    auto loaded = load_throttle(path);
    EXPECT_TRUE(loaded.exists);
    EXPECT_EQ(loaded.last_seen_version, "0.2.0");
    fs::remove(path);
}

TEST(UpdatePromptFlow, NoPromptWhenAlreadyUpToDate) {
    auto path = tmp_state_path("uptodate");
    fs::remove(path);
    UpdateConfig cfg;
    cfg.current_version = "0.5.0";
    cfg.state_path_override = path;
    cfg.force = true;
    cfg.fetch = [](const std::string&) -> std::optional<std::string> {
        return std::string(R"({"version":"0.5.0"})");
    };
    bool prompted = false;
    cfg.write_line = [&](std::string_view) { prompted = true; };
    cfg.read_line = []() -> std::optional<std::string> {
        ADD_FAILURE() << "read_line should not be called when up to date";
        return std::nullopt;
    };
    auto out = maybe_prompt_update(cfg);
    EXPECT_FALSE(out.prompted);
    EXPECT_FALSE(prompted);
    EXPECT_EQ(out.detail, "up to date");
    fs::remove(path);
}

TEST(UpdatePromptFlow, FetchFailureStillStampsThrottle) {
    auto path = tmp_state_path("fetchfail");
    fs::remove(path);
    UpdateConfig cfg;
    cfg.current_version = "0.1.0";
    cfg.state_path_override = path;
    cfg.force = true;
    cfg.fetch = [](const std::string&) -> std::optional<std::string> {
        return std::nullopt;
    };
    auto out = maybe_prompt_update(cfg);
    EXPECT_FALSE(out.prompted);
    auto loaded = load_throttle(path);
    EXPECT_TRUE(loaded.exists);  // throttle stamped to avoid hammering
    fs::remove(path);
}
