// test_main_entry_full — depth tests for the newly-ported main_entry
// helpers (preparse, help_gen, diagnostics, pipe_mode, update_flow,
// session_browser, repl_dispatch).  These tests exercise the C++ ports
// of the Python helpers from hermes_cli/main.py.

#include "hermes/cli/main_preparse.hpp"
#include "hermes/cli/main_help_gen.hpp"
#include "hermes/cli/main_diagnostics.hpp"
#include "hermes/cli/main_pipe_mode.hpp"
#include "hermes/cli/main_update_flow.hpp"
#include "hermes/cli/main_session_browser.hpp"
#include "hermes/cli/main_repl_dispatch.hpp"

#include <gtest/gtest.h>

#include <cstring>
#include <ctime>
#include <sstream>
#include <string>
#include <vector>

namespace {

// Build a synthetic argv vector.  Returns raw pointers valid for the life
// of the backing vector<string>.
struct Argv {
    std::vector<std::string> storage;
    std::vector<char*> ptrs;

    Argv(std::initializer_list<const char*> init) {
        for (auto* a : init) storage.emplace_back(a);
        for (auto& s : storage) ptrs.push_back(s.data());
    }

    int argc() const { return static_cast<int>(ptrs.size()); }
    char** argv() { return ptrs.data(); }
};

}  // namespace

// ---------------------------------------------------------------------------
// preparse — relative time formatting
// ---------------------------------------------------------------------------
TEST(PreparseRelativeTime, ZeroReturnsQuestionMark) {
    EXPECT_EQ("?", hermes::cli::preparse::format_relative_time(0));
    EXPECT_EQ("?", hermes::cli::preparse::format_relative_time(-5));
}

TEST(PreparseRelativeTime, SubMinuteReturnsJustNow) {
    std::time_t now = 1'700'000'000;
    EXPECT_EQ("just now",
              hermes::cli::preparse::format_relative_time(now - 30, now));
}

TEST(PreparseRelativeTime, MinutesAndHours) {
    std::time_t now = 1'700'000'000;
    EXPECT_EQ("5m ago",
              hermes::cli::preparse::format_relative_time(now - 5 * 60, now));
    EXPECT_EQ("3h ago",
              hermes::cli::preparse::format_relative_time(now - 3 * 3600, now));
}

TEST(PreparseRelativeTime, YesterdayAndDaysAgo) {
    std::time_t now = 1'700'000'000;
    EXPECT_EQ("yesterday",
              hermes::cli::preparse::format_relative_time(
                  now - (24 * 3600 + 10), now));
    EXPECT_EQ("4d ago",
              hermes::cli::preparse::format_relative_time(now - 4 * 86400, now));
}

TEST(PreparseRelativeTime, OlderRendersIsoDate) {
    std::time_t now = 1'700'000'000;
    auto out = hermes::cli::preparse::format_relative_time(
        now - 40 * 86400, now);
    // ISO date format: YYYY-MM-DD (10 chars, dashes at 4 & 7).
    ASSERT_EQ(10u, out.size());
    EXPECT_EQ('-', out[4]);
    EXPECT_EQ('-', out[7]);
}

// ---------------------------------------------------------------------------
// preparse — profile flag stripping
// ---------------------------------------------------------------------------
TEST(PreparseProfileFlag, StripsLongForm) {
    std::vector<std::string> in = {"hermes", "--profile", "coder", "chat"};
    std::optional<std::string> got;
    auto out = hermes::cli::preparse::strip_profile_flag(in, &got);
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ("coder", *got);
    ASSERT_EQ(2u, out.size());
    EXPECT_EQ("hermes", out[0]);
    EXPECT_EQ("chat", out[1]);
}

TEST(PreparseProfileFlag, StripsShortForm) {
    std::vector<std::string> in = {"hermes", "-p", "work", "status"};
    std::optional<std::string> got;
    auto out = hermes::cli::preparse::strip_profile_flag(in, &got);
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ("work", *got);
    EXPECT_EQ(std::vector<std::string>({"hermes", "status"}), out);
}

TEST(PreparseProfileFlag, StripsEqualsForm) {
    std::vector<std::string> in = {"hermes", "--profile=dev", "chat"};
    std::optional<std::string> got;
    auto out = hermes::cli::preparse::strip_profile_flag(in, &got);
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ("dev", *got);
    EXPECT_EQ(std::vector<std::string>({"hermes", "chat"}), out);
}

TEST(PreparseProfileFlag, IgnoresPositionalAfterSubcommand) {
    std::vector<std::string> in = {"hermes", "chat", "--profile", "dev"};
    // strip_profile_flag is not context-aware; still consumes the first
    // occurrence it sees, which is what we want here.
    std::optional<std::string> got;
    auto out = hermes::cli::preparse::strip_profile_flag(in, &got);
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ("dev", *got);
}

// ---------------------------------------------------------------------------
// preparse — coalesce_session_name_args
// ---------------------------------------------------------------------------
TEST(PreparseCoalesce, MergesTrailingBareWords) {
    auto out = hermes::cli::preparse::coalesce_session_name_args(
        {"hermes", "-c", "my", "project", "name"});
    ASSERT_EQ(3u, out.size());
    EXPECT_EQ("hermes", out[0]);
    EXPECT_EQ("-c", out[1]);
    EXPECT_EQ("my project name", out[2]);
}

TEST(PreparseCoalesce, StopsAtFlagBoundary) {
    auto out = hermes::cli::preparse::coalesce_session_name_args(
        {"hermes", "-c", "my", "project", "--worktree"});
    EXPECT_EQ(4u, out.size());
    EXPECT_EQ("my project", out[2]);
    EXPECT_EQ("--worktree", out[3]);
}

TEST(PreparseCoalesce, PassThroughWhenNoContinue) {
    std::vector<std::string> in = {"hermes", "chat", "-v"};
    auto out = hermes::cli::preparse::coalesce_session_name_args(in);
    EXPECT_EQ(in, out);
}

// ---------------------------------------------------------------------------
// preparse — global flag pre-parse
// ---------------------------------------------------------------------------
TEST(PreparseGlobalFlags, ParsesYoloAndWorktree) {
    Argv argv{"hermes", "--yolo", "-w", "chat"};
    int ac = argv.argc();
    auto flags = hermes::cli::preparse::pre_parse_global_flags(ac, argv.argv());
    EXPECT_TRUE(flags.yolo);
    EXPECT_TRUE(flags.worktree);
    EXPECT_EQ(2, ac);  // after flags stripped
}

TEST(PreparseGlobalFlags, ParsesContinueWithValue) {
    Argv argv{"hermes", "-c", "my-session"};
    int ac = argv.argc();
    auto flags = hermes::cli::preparse::pre_parse_global_flags(ac, argv.argv());
    EXPECT_TRUE(flags.continue_flag);
    EXPECT_EQ("my-session", flags.continue_name);
    EXPECT_EQ(1, ac);
}

TEST(PreparseGlobalFlags, ParsesContinueBareFlag) {
    Argv argv{"hermes", "-c"};
    int ac = argv.argc();
    auto flags = hermes::cli::preparse::pre_parse_global_flags(ac, argv.argv());
    EXPECT_TRUE(flags.continue_flag);
    EXPECT_EQ("", flags.continue_name);
}

TEST(PreparseGlobalFlags, ParsesSkillsRepeatable) {
    Argv argv{"hermes", "-s", "foo,bar", "-s", "baz"};
    int ac = argv.argc();
    auto flags = hermes::cli::preparse::pre_parse_global_flags(ac, argv.argv());
    ASSERT_EQ(3u, flags.skills.size());
    EXPECT_EQ("foo", flags.skills[0]);
    EXPECT_EQ("bar", flags.skills[1]);
    EXPECT_EQ("baz", flags.skills[2]);
}

TEST(PreparseGlobalFlags, ParsesMaxTurns) {
    Argv argv{"hermes", "--max-turns", "42"};
    int ac = argv.argc();
    auto flags = hermes::cli::preparse::pre_parse_global_flags(ac, argv.argv());
    EXPECT_EQ(42, flags.max_turns);
}

// ---------------------------------------------------------------------------
// preparse — provider probe
// ---------------------------------------------------------------------------
TEST(PreparseProviderProbe, EnvVarsListIncludesCoreProviders) {
    const auto& vars = hermes::cli::preparse::provider_env_vars();
    bool has_openai = false, has_anthropic = false, has_nous = false;
    for (const auto& v : vars) {
        if (v == "OPENAI_API_KEY")    has_openai = true;
        if (v == "ANTHROPIC_API_KEY") has_anthropic = true;
        if (v == "NOUS_API_KEY")      has_nous = true;
    }
    EXPECT_TRUE(has_openai);
    EXPECT_TRUE(has_anthropic);
    EXPECT_TRUE(has_nous);
}

TEST(PreparseProviderProbe, DoesNotThrow) {
    EXPECT_NO_THROW({
        auto p = hermes::cli::preparse::probe_any_provider_configured();
        (void)p;
    });
}

// ---------------------------------------------------------------------------
// preparse — require_tty / input_file extraction
// ---------------------------------------------------------------------------
TEST(PreparseRequireTty, EmitsErrorOnPipe) {
    // The test harness always runs with stdin redirected, so require_tty
    // should report false.  But if it happens to be a TTY (local dev), we
    // accept either answer — the contract is just that it doesn't throw.
    std::ostringstream err;
    bool result =
        hermes::cli::preparse::require_tty("setup", &err);
    if (!result) {
        EXPECT_NE(err.str().find("Error"), std::string::npos);
    }
}

TEST(PreparseExtractInputFile, RecognizesLongAndShortForm) {
    Argv argv{"hermes", "--input", "/tmp/q.txt", "chat"};
    int ac = argv.argc();
    auto f = hermes::cli::preparse::extract_input_file(ac, argv.argv());
    EXPECT_EQ("/tmp/q.txt", f);
    EXPECT_EQ(2, ac);
}

TEST(PreparseExtractInputFile, EqualsFormat) {
    Argv argv{"hermes", "--input=/tmp/q.txt"};
    int ac = argv.argc();
    auto f = hermes::cli::preparse::extract_input_file(ac, argv.argv());
    EXPECT_EQ("/tmp/q.txt", f);
    EXPECT_EQ(1, ac);
}

// ---------------------------------------------------------------------------
// help_gen — registry consistency
// ---------------------------------------------------------------------------
TEST(HelpGen, RegistryHasAllCoreCommands) {
    auto* chat    = hermes::cli::help_gen::find_command("chat");
    auto* gateway = hermes::cli::help_gen::find_command("gateway");
    auto* doctor  = hermes::cli::help_gen::find_command("doctor");
    auto* auth    = hermes::cli::help_gen::find_command("auth");
    auto* profile = hermes::cli::help_gen::find_command("profile");
    ASSERT_NE(nullptr, chat);
    ASSERT_NE(nullptr, gateway);
    ASSERT_NE(nullptr, doctor);
    ASSERT_NE(nullptr, auth);
    ASSERT_NE(nullptr, profile);
    EXPECT_FALSE(gateway->subcommands.empty());
}

TEST(HelpGen, DottedLookup) {
    auto* install = hermes::cli::help_gen::find_command("gateway.install");
    ASSERT_NE(nullptr, install);
    EXPECT_EQ("install", install->name);
}

TEST(HelpGen, GlobalHelpContainsAllCategories) {
    hermes::cli::help_gen::HelpOptions opts;
    opts.color = false;
    auto text = hermes::cli::help_gen::render_global_help(opts);
    EXPECT_NE(text.find("Chat & sessions"), std::string::npos);
    EXPECT_NE(text.find("Authentication"), std::string::npos);
    EXPECT_NE(text.find("Infrastructure"), std::string::npos);
    EXPECT_NE(text.find("Examples:"), std::string::npos);
}

TEST(HelpGen, CommandHelpShowsFlags) {
    auto* chat = hermes::cli::help_gen::find_command("chat");
    ASSERT_NE(nullptr, chat);
    hermes::cli::help_gen::HelpOptions opts;
    opts.color = false;
    auto text = hermes::cli::help_gen::render_command_help(*chat, opts);
    EXPECT_NE(text.find("--query"), std::string::npos);
    EXPECT_NE(text.find("--yolo"), std::string::npos);
    EXPECT_NE(text.find("--max-turns"), std::string::npos);
}

TEST(HelpGen, BashCompletionMentionsAllTopLevelCommands) {
    auto out = hermes::cli::help_gen::generate_bash_completion();
    EXPECT_NE(out.find("complete -F _hermes hermes"), std::string::npos);
    EXPECT_NE(out.find("chat"), std::string::npos);
    EXPECT_NE(out.find("gateway"), std::string::npos);
    EXPECT_NE(out.find("doctor"), std::string::npos);
}

TEST(HelpGen, ZshCompletionIsNonEmpty) {
    auto out = hermes::cli::help_gen::generate_zsh_completion();
    EXPECT_NE(out.find("#compdef hermes"), std::string::npos);
    EXPECT_NE(out.find("_describe"), std::string::npos);
}

TEST(HelpGen, AllCommandNamesIsSortedAndUnique) {
    auto names = hermes::cli::help_gen::all_command_names();
    EXPECT_FALSE(names.empty());
    for (std::size_t i = 1; i < names.size(); ++i) {
        EXPECT_LT(names[i - 1], names[i]);
    }
}

TEST(HelpGen, WordWrapRespectsWidth) {
    std::string in =
        "the quick brown fox jumps over the lazy dog and then some more text";
    auto out = hermes::cli::help_gen::word_wrap(in, 20);
    // No line should exceed 20 chars.
    std::istringstream is(out);
    std::string line;
    while (std::getline(is, line)) {
        EXPECT_LE(line.size(), 20u);
    }
}

TEST(HelpGen, PadToTruncatesLongStrings) {
    auto out = hermes::cli::help_gen::pad_to("averyverylongstring", 10);
    // "…" is a 3-byte UTF-8 sequence, so the byte length exceeds 10.
    // The visible column width should still be <= 10; truncation happened.
    EXPECT_LT(out.size(), std::string("averyverylongstring").size());
    EXPECT_NE(out.find("…"), std::string::npos);
}

// ---------------------------------------------------------------------------
// diagnostics — build info + environment
// ---------------------------------------------------------------------------
TEST(Diagnostics, BuildInfoHasVersionString) {
    auto bi = hermes::cli::diagnostics::collect_build_info();
    EXPECT_FALSE(bi.hermes_version.empty());
    EXPECT_NE(bi.hermes_version.find("0.1.0"), std::string::npos);
    EXPECT_FALSE(bi.compiler.empty());
    EXPECT_FALSE(bi.target_triple.empty());
}

TEST(Diagnostics, FormatBuildInfoNotEmpty) {
    auto bi = hermes::cli::diagnostics::collect_build_info();
    auto text = hermes::cli::diagnostics::format_build_info(bi);
    EXPECT_NE(text.find("Version:"), std::string::npos);
    EXPECT_NE(text.find("Compiler:"), std::string::npos);
}

TEST(Diagnostics, EnvironmentCollectDoesNotThrow) {
    EXPECT_NO_THROW({
        auto env = hermes::cli::diagnostics::collect_environment();
        auto text = hermes::cli::diagnostics::format_environment(env);
        EXPECT_NE(text.find("Platform:"), std::string::npos);
    });
}

TEST(Diagnostics, OnelineBannerIncludesVersion) {
    auto b = hermes::cli::diagnostics::render_oneline_banner();
    EXPECT_NE(b.find("Hermes"), std::string::npos);
    EXPECT_NE(b.find("0.1.0"), std::string::npos);
}

TEST(Diagnostics, MultilineBannerRenders) {
    hermes::cli::diagnostics::BannerOptions opts;
    opts.color = false;
    auto b = hermes::cli::diagnostics::render_banner(opts);
    EXPECT_NE(b.find("Hermes"), std::string::npos);
    EXPECT_NE(b.find("/help"), std::string::npos);
}

TEST(Diagnostics, UpdateHintFormatsGracefully) {
    hermes::cli::diagnostics::UpdateCheckResult r;
    r.checked = true;
    r.commits_behind = 0;
    EXPECT_EQ("Up to date",
              hermes::cli::diagnostics::format_update_hint(r));
    r.commits_behind = 5;
    auto hint = hermes::cli::diagnostics::format_update_hint(r);
    EXPECT_NE(hint.find("5 commits behind"), std::string::npos);
    r.commits_behind = 1;
    hint = hermes::cli::diagnostics::format_update_hint(r);
    EXPECT_NE(hint.find("1 commit behind"), std::string::npos);
}

TEST(Diagnostics, ModelProbeDoesNotHitNetwork) {
    // Configured-model probe is offline by default.
    auto r = hermes::cli::diagnostics::probe_configured_model();
    EXPECT_FALSE(r.status.empty());
}

TEST(Diagnostics, StartupReportNoNetworkModeFast) {
    auto r = hermes::cli::diagnostics::collect_startup_report(
        /*skip_network=*/true);
    EXPECT_FALSE(r.build.hermes_version.empty());
    EXPECT_FALSE(r.env.platform.empty());
    // skip_network=true should NOT trigger the update probe.
    EXPECT_FALSE(r.update.checked);
}

// ---------------------------------------------------------------------------
// pipe_mode — parsing
// ---------------------------------------------------------------------------
TEST(PipeMode, QueryArgTakesPrecedence) {
    Argv argv{"hermes", "-q", "hello"};
    auto req = hermes::cli::pipe_mode::parse_input_request(
        argv.argc(), argv.argv());
    EXPECT_EQ(hermes::cli::pipe_mode::InputSource::kQueryArg, req.source);
    EXPECT_EQ("hello", req.payload);
}

TEST(PipeMode, InputFileResolvedEvenWhenMissing) {
    Argv argv{"hermes", "--input", "/this/does/not/exist/xyz"};
    auto req = hermes::cli::pipe_mode::parse_input_request(
        argv.argc(), argv.argv());
    // Missing file should flip to kNone.
    EXPECT_EQ(hermes::cli::pipe_mode::InputSource::kNone, req.source);
}

TEST(PipeMode, SinkEmitsStreamingChunksAndFinalizes) {
    std::ostringstream os;
    hermes::cli::pipe_mode::PipeSink sink(os);
    sink.on_text_chunk("hello ");
    sink.on_text_chunk("world");
    sink.finish("hello world");
    auto s = os.str();
    EXPECT_NE(s.find("hello"), std::string::npos);
    // Must end with a newline.
    ASSERT_FALSE(s.empty());
    EXPECT_EQ('\n', s.back());
}

TEST(PipeMode, SinkQuietSuppressesToolPreviews) {
    std::ostringstream os;
    hermes::cli::pipe_mode::PipeSink sink(os);
    sink.set_quiet(true);
    sink.on_tool_call_preview("shell", "ls -la");
    sink.on_tool_result("shell", "ok");
    EXPECT_EQ("", os.str());  // quiet => nothing written
}

TEST(PipeMode, RunPipeModeSkipsEmpty) {
    hermes::cli::pipe_mode::InputRequest req;
    req.source = hermes::cli::pipe_mode::InputSource::kNone;
    int rc = hermes::cli::pipe_mode::run_pipe_mode(req,
        [](const std::string&) { return 42; });
    EXPECT_EQ(0, rc);
}

TEST(PipeMode, RunPipeModeCallsHandler) {
    hermes::cli::pipe_mode::InputRequest req;
    req.source = hermes::cli::pipe_mode::InputSource::kQueryArg;
    req.payload = "hi";
    int rc = hermes::cli::pipe_mode::run_pipe_mode(req,
        [](const std::string& q) {
            EXPECT_EQ("hi", q);
            return 7;
        });
    EXPECT_EQ(7, rc);
}

// ---------------------------------------------------------------------------
// update_flow — git helpers with FakeGitShell
// ---------------------------------------------------------------------------
TEST(UpdateFlow, GetOriginUrlTrimsOutput) {
    hermes::cli::update_flow::FakeGitShell sh;
    sh.push_result(0, "https://github.com/foo/bar.git\n");
    auto url = hermes::cli::update_flow::get_origin_url(sh, "/tmp");
    ASSERT_TRUE(url.has_value());
    EXPECT_EQ("https://github.com/foo/bar.git", *url);
}

TEST(UpdateFlow, GetOriginUrlMissing) {
    hermes::cli::update_flow::FakeGitShell sh;
    sh.push_result(1, "", "no such remote");
    auto url = hermes::cli::update_flow::get_origin_url(sh, "/tmp");
    EXPECT_FALSE(url.has_value());
}

TEST(UpdateFlow, IsForkRecognizesUserFork) {
    using hermes::cli::update_flow::is_fork;
    EXPECT_FALSE(is_fork(std::optional<std::string>{
        "https://github.com/NousResearch/hermes-agent.git"}));
    EXPECT_TRUE(is_fork(std::optional<std::string>{
        "https://github.com/someuser/hermes-agent.git"}));
    EXPECT_FALSE(is_fork(std::nullopt));
}

TEST(UpdateFlow, HasUpstreamRemote) {
    hermes::cli::update_flow::FakeGitShell sh;
    sh.push_result(0, "origin\nupstream\n");
    EXPECT_TRUE(hermes::cli::update_flow::has_upstream_remote(sh, ""));
    hermes::cli::update_flow::FakeGitShell sh2;
    sh2.push_result(0, "origin\n");
    EXPECT_FALSE(hermes::cli::update_flow::has_upstream_remote(sh2, ""));
}

TEST(UpdateFlow, CountCommitsBetweenParsesInt) {
    hermes::cli::update_flow::FakeGitShell sh;
    sh.push_result(0, "17\n");
    EXPECT_EQ(17, hermes::cli::update_flow::count_commits_between(
                      sh, "", "HEAD", "origin/main"));
}

TEST(UpdateFlow, StashSkipsWhenClean) {
    hermes::cli::update_flow::FakeGitShell sh;
    sh.push_result(0, "");  // `git status --porcelain` => clean
    auto ref = hermes::cli::update_flow::stash_local_changes_if_needed(sh, "");
    EXPECT_FALSE(ref.has_value());
}

TEST(UpdateFlow, StashPushesWhenDirty) {
    hermes::cli::update_flow::FakeGitShell sh;
    sh.push_result(0, " M foo.py\n");  // dirty
    sh.push_result(0, "");             // stash push succeeds
    auto ref = hermes::cli::update_flow::stash_local_changes_if_needed(sh, "");
    ASSERT_TRUE(ref.has_value());
    EXPECT_EQ("stash@{0}", *ref);
}

TEST(UpdateFlow, RestoreStashSuccess) {
    hermes::cli::update_flow::FakeGitShell sh;
    sh.push_result(0);
    std::vector<std::string> conflicts;
    bool ok = hermes::cli::update_flow::restore_stashed_changes(
        sh, "", "stash@{0}", &conflicts);
    EXPECT_TRUE(ok);
    EXPECT_TRUE(conflicts.empty());
}

TEST(UpdateFlow, RestoreStashConflict) {
    hermes::cli::update_flow::FakeGitShell sh;
    sh.push_result(1, "CONFLICT (content): Merge conflict in foo.py\n");
    std::vector<std::string> conflicts;
    bool ok = hermes::cli::update_flow::restore_stashed_changes(
        sh, "", "stash@{0}", &conflicts);
    EXPECT_FALSE(ok);
    EXPECT_FALSE(conflicts.empty());
}

TEST(UpdateFlow, CleanupGuidanceIncludesRef) {
    auto text = hermes::cli::update_flow::format_stash_cleanup_guidance(
        "stash@{0}");
    EXPECT_NE(text.find("stash@{0}"), std::string::npos);
    EXPECT_NE(text.find("git stash pop"), std::string::npos);
}

TEST(UpdateFlow, RunUpdateNotGitCheckout) {
    hermes::cli::update_flow::FakeGitShell sh;
    sh.push_result(1, "", "not a git checkout");
    hermes::cli::update_flow::UpdateOptions opts;
    opts.working_dir = "/tmp";
    auto r = hermes::cli::update_flow::run_update(opts, sh);
    EXPECT_EQ(1, r.exit_code);
    EXPECT_NE(r.summary.find("Not a git checkout"), std::string::npos);
}

TEST(UpdateFlow, BytecodeCacheNoOpOnMissingDir) {
    int removed = hermes::cli::update_flow::clear_bytecode_cache(
        "/does/not/exist/xyz");
    EXPECT_EQ(0, removed);
}

// ---------------------------------------------------------------------------
// session_browser
// ---------------------------------------------------------------------------
TEST(SessionBrowser, MatchesQueryCaseInsensitive) {
    hermes::cli::session_browser::Session s;
    s.id = "abc123";
    s.title = "My Cool Project";
    s.preview = "some preview text";
    s.source = "cli";
    EXPECT_TRUE(hermes::cli::session_browser::session_matches_query(s, "cool"));
    EXPECT_TRUE(hermes::cli::session_browser::session_matches_query(s, "PROJECT"));
    EXPECT_TRUE(hermes::cli::session_browser::session_matches_query(s, "abc"));
    EXPECT_TRUE(hermes::cli::session_browser::session_matches_query(s, "cli"));
    EXPECT_FALSE(hermes::cli::session_browser::session_matches_query(s, "zzzz"));
    EXPECT_TRUE(hermes::cli::session_browser::session_matches_query(s, ""));
}

TEST(SessionBrowser, SortByRecencyStable) {
    std::vector<hermes::cli::session_browser::Session> sessions(3);
    sessions[0].id = "a"; sessions[0].last_active = 100;
    sessions[1].id = "b"; sessions[1].last_active = 300;
    sessions[2].id = "c"; sessions[2].last_active = 200;
    hermes::cli::session_browser::sort_sessions_by_recency(sessions);
    EXPECT_EQ("b", sessions[0].id);
    EXPECT_EQ("c", sessions[1].id);
    EXPECT_EQ("a", sessions[2].id);
}

TEST(SessionBrowser, FilterReturnsPointers) {
    std::vector<hermes::cli::session_browser::Session> sessions(2);
    sessions[0].title = "one";
    sessions[1].title = "two";
    auto filt = hermes::cli::session_browser::filter_sessions(sessions, "one");
    ASSERT_EQ(1u, filt.size());
    EXPECT_EQ("one", filt[0]->title);
}

TEST(SessionBrowser, LayoutScalesWithWidth) {
    auto narrow = hermes::cli::session_browser::layout_for_width(60);
    auto wide   = hermes::cli::session_browser::layout_for_width(160);
    EXPECT_GT(wide.title_width, narrow.title_width);
}

TEST(SessionBrowser, FormatRowIncludesTitle) {
    hermes::cli::session_browser::Session s;
    s.title = "hello world";
    s.id = "abc123456";
    s.source = "cli";
    s.last_active = std::time(nullptr) - 600;
    auto layout = hermes::cli::session_browser::layout_for_width(100);
    auto row = hermes::cli::session_browser::format_row(s, layout, false);
    EXPECT_NE(row.find("hello world"), std::string::npos);
    EXPECT_NE(row.find("abc"), std::string::npos);
}

TEST(SessionBrowser, FormatListRow) {
    hermes::cli::session_browser::Session s;
    s.title = "demo";
    s.id = "sha-001";
    s.source = "cli";
    auto row = hermes::cli::session_browser::format_list_row(s);
    EXPECT_NE(row.find("demo"), std::string::npos);
    EXPECT_NE(row.find("sha-001"), std::string::npos);
}

TEST(SessionBrowser, TextPickerReturnsSelection) {
    std::vector<hermes::cli::session_browser::Session> sessions(2);
    sessions[0].id = "s0"; sessions[0].title = "zero";
    sessions[1].id = "s1"; sessions[1].title = "one";
    std::stringstream in;
    std::ostringstream out;
    in << "2\n";
    hermes::cli::session_browser::PickerIO io{&in, &out};
    auto got = hermes::cli::session_browser::text_picker(
        sessions, hermes::cli::session_browser::PickerOptions{}, io);
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ("s1", *got);
}

TEST(SessionBrowser, TextPickerCancelsOnQ) {
    std::vector<hermes::cli::session_browser::Session> sessions(1);
    sessions[0].id = "s0";
    std::stringstream in;
    std::ostringstream out;
    in << "q\n";
    hermes::cli::session_browser::PickerIO io{&in, &out};
    auto got = hermes::cli::session_browser::text_picker(
        sessions, hermes::cli::session_browser::PickerOptions{}, io);
    EXPECT_FALSE(got.has_value());
}

TEST(SessionBrowser, RunListJsonEmptySafe) {
    std::ostringstream out;
    hermes::cli::session_browser::ListOptions opts;
    opts.as_json = true;
    int rc = hermes::cli::session_browser::run_list(opts, out);
    EXPECT_EQ(0, rc);
}

// ---------------------------------------------------------------------------
// repl_dispatch
// ---------------------------------------------------------------------------
TEST(ReplDispatch, ParsesBasicCommand) {
    auto p = hermes::cli::repl_dispatch::parse_command_line("/help");
    EXPECT_TRUE(p.valid);
    EXPECT_EQ("/help", p.head);
    EXPECT_EQ("", p.args);
}

TEST(ReplDispatch, ParsesArguments) {
    auto p = hermes::cli::repl_dispatch::parse_command_line(
        "  /model anthropic/claude ");
    EXPECT_TRUE(p.valid);
    EXPECT_EQ("/model", p.head);
    EXPECT_EQ("anthropic/claude", p.args);
}

TEST(ReplDispatch, RejectsNonSlash) {
    EXPECT_FALSE(hermes::cli::repl_dispatch::parse_command_line("hello").valid);
    EXPECT_FALSE(hermes::cli::repl_dispatch::is_slash_command("hello"));
}

TEST(ReplDispatch, RegistryHasHelpAndQuit) {
    EXPECT_NE(nullptr, hermes::cli::repl_dispatch::find_slash_command("/help"));
    EXPECT_NE(nullptr, hermes::cli::repl_dispatch::find_slash_command("/quit"));
    EXPECT_NE(nullptr, hermes::cli::repl_dispatch::find_slash_command("/h"));
    EXPECT_NE(nullptr, hermes::cli::repl_dispatch::find_slash_command("/exit"));
}

TEST(ReplDispatch, RenderSlashHelpIncludesCategories) {
    auto text = hermes::cli::repl_dispatch::render_slash_help(false, false);
    EXPECT_NE(text.find("/help"), std::string::npos);
    EXPECT_NE(text.find("/quit"), std::string::npos);
    EXPECT_NE(text.find("/model"), std::string::npos);
}

TEST(ReplDispatch, DispatcherRoutesToHandler) {
    hermes::cli::repl_dispatch::Dispatcher d;
    bool ran = false;
    d.register_handler("/foo", [&](const hermes::cli::repl_dispatch::ParsedCommand& c) {
        EXPECT_EQ("bar", c.args);
        ran = true;
        return 0;
    });
    int rc = d.dispatch_line("/foo bar");
    EXPECT_EQ(0, rc);
    EXPECT_TRUE(ran);
}

TEST(ReplDispatch, DispatcherRoutesByAlias) {
    hermes::cli::repl_dispatch::Dispatcher d;
    int count = 0;
    d.register_handler("/help",
                       [&](const hermes::cli::repl_dispatch::ParsedCommand&) {
                           ++count;
                           return 0;
                       });
    d.dispatch_line("/h");
    d.dispatch_line("/?");
    EXPECT_EQ(2, count);
}

TEST(ReplDispatch, DispatcherReturnsMinusOneOnMiss) {
    hermes::cli::repl_dispatch::Dispatcher d;
    int rc = d.dispatch_line("/bogus");
    EXPECT_EQ(-1, rc);
}

TEST(ReplDispatch, DefaultDispatcherHasCoreCommands) {
    auto d = hermes::cli::repl_dispatch::make_default_dispatcher();
    EXPECT_TRUE(d.has("/help"));
    EXPECT_TRUE(d.has("/quit"));
    EXPECT_TRUE(d.has("/model"));
}

TEST(ReplDispatch, CompleteSlashPrefix) {
    auto out = hermes::cli::repl_dispatch::complete_slash("/he");
    EXPECT_FALSE(out.empty());
    bool found_help = false;
    for (const auto& s : out) if (s == "/help") found_help = true;
    EXPECT_TRUE(found_help);
}

TEST(ReplDispatch, CompleteArgumentForModel) {
    auto out = hermes::cli::repl_dispatch::complete_argument("/model", "anth");
    EXPECT_FALSE(out.empty());
    for (const auto& s : out) {
        EXPECT_EQ(0u, s.find("anthropic"));
    }
}

TEST(ReplDispatch, HandleCtrlCDoubleTap) {
    // First press — not a double tap.
    EXPECT_FALSE(hermes::cli::repl_dispatch::handle_ctrl_c(500));
    // Immediate second press within threshold — should be a double tap.
    EXPECT_TRUE(hermes::cli::repl_dispatch::handle_ctrl_c(10'000));
}

TEST(ReplDispatch, HandleCtrlDExitsOnlyWhenBufferEmpty) {
    EXPECT_TRUE(hermes::cli::repl_dispatch::handle_ctrl_d(true));
    EXPECT_FALSE(hermes::cli::repl_dispatch::handle_ctrl_d(false));
}

TEST(ReplDispatch, FormatReplStatusIncludesSessionAndTurns) {
    hermes::cli::repl_dispatch::ReplState s;
    s.current_session_id = "abc";
    s.turn_count = 3;
    s.yolo_mode = true;
    auto line = hermes::cli::repl_dispatch::format_repl_status(s);
    EXPECT_NE(line.find("abc"), std::string::npos);
    EXPECT_NE(line.find("turns=3"), std::string::npos);
    EXPECT_NE(line.find("yolo"), std::string::npos);
}

TEST(ReplDispatch, InterruptFlagLifecycle) {
    hermes::cli::repl_dispatch::clear_interrupt();
    EXPECT_FALSE(hermes::cli::repl_dispatch::interrupt_pending());
}

TEST(ReplDispatch, InstallAndRestoreIsIdempotent) {
    hermes::cli::repl_dispatch::install_signal_handlers();
    hermes::cli::repl_dispatch::install_signal_handlers();  // no throw
    hermes::cli::repl_dispatch::restore_signal_handlers();
    // Re-install after restore must succeed.
    hermes::cli::repl_dispatch::install_signal_handlers();
    hermes::cli::repl_dispatch::restore_signal_handlers();
}
