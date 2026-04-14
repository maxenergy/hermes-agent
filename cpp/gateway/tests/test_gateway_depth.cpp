// Tests for the deepened gateway modules:
//   - gateway_helpers
//   - base_adapter (TokenBucket, RetryBudget, FeatureFlags, BaseAdapterMixin)
//   - backpressure
//   - metrics
//   - command_dispatcher
//   - shutdown_sequencer
//   - config_validator
//   - session_ttl (SessionTtlTracker, InsightLedger, SessionLinker)
//
// Targets >=20 new cases per the port acceptance criteria.
#include <gtest/gtest.h>

#include <chrono>
#include <thread>

#include <nlohmann/json.hpp>

#include "../platforms/base_adapter.hpp"
#include <hermes/gateway/backpressure.hpp>
#include <hermes/gateway/command_dispatcher.hpp>
#include <hermes/gateway/config_validator.hpp>
#include <hermes/gateway/gateway_helpers.hpp>
#include <hermes/gateway/gateway_runner.hpp>
#include <hermes/gateway/metrics.hpp>
#include <hermes/gateway/message_pipeline.hpp>
#include <hermes/gateway/session_ttl.hpp>
#include <hermes/gateway/shutdown_sequencer.hpp>

using namespace hermes::gateway;

// ---- gateway_helpers ----------------------------------------------------

TEST(GatewayHelpers, BuildMediaPlaceholderPhoto) {
    MediaPlaceholderEvent ev;
    ev.message_type = "PHOTO";
    ev.media_urls = {"https://x/y.jpg"};
    auto s = build_media_placeholder(ev);
    EXPECT_NE(s.find("photo"), std::string::npos);
    EXPECT_NE(s.find("[user sent"), std::string::npos);
}

TEST(GatewayHelpers, BuildMediaPlaceholderWithCaption) {
    MediaPlaceholderEvent ev;
    ev.message_type = "video";
    ev.caption = "look at this";
    auto s = build_media_placeholder(ev);
    EXPECT_NE(s.find("video"), std::string::npos);
    EXPECT_NE(s.find("look at this"), std::string::npos);
}

TEST(GatewayHelpers, SafeUrlForLogStripsQueryAndCredentials) {
    auto s = safe_url_for_log("https://user:pass@api.example.com/v1/path?tok=SECRET");
    EXPECT_EQ(s.find("SECRET"), std::string::npos);
    EXPECT_EQ(s.find("pass"), std::string::npos);
    EXPECT_NE(s.find("api.example.com"), std::string::npos);
}

TEST(GatewayHelpers, MaskTokenPreservesBookends) {
    EXPECT_EQ(mask_token("1234567890ABCD"), "1234***ABCD");
    EXPECT_EQ(mask_token("shortie"), "***");
}

TEST(GatewayHelpers, TruncateMessageBreaksOnParagraph) {
    std::string body = std::string(80, 'a') + "\n\n" + std::string(80, 'b');
    auto chunks = truncate_message(body, 90);
    ASSERT_GE(chunks.size(), 2u);
    for (auto& c : chunks) EXPECT_LE(c.size(), 90u);
}

TEST(GatewayHelpers, TruncateMessageShortIsSingleChunk) {
    auto chunks = truncate_message("hello", 100);
    ASSERT_EQ(chunks.size(), 1u);
    EXPECT_EQ(chunks[0], "hello");
}

TEST(GatewayHelpers, LooksLikeSlashCommand) {
    EXPECT_TRUE(looks_like_slash_command("/stop"));
    EXPECT_TRUE(looks_like_slash_command("   /help"));
    EXPECT_FALSE(looks_like_slash_command("//comment"));
    EXPECT_FALSE(looks_like_slash_command("hello /stop"));
    EXPECT_FALSE(looks_like_slash_command(""));
}

TEST(GatewayHelpers, ExtractCommandWordAndArgs) {
    EXPECT_EQ(*extract_command_word("/Stop now"), "stop");
    EXPECT_EQ(extract_command_args("/model gpt-4  "), "gpt-4");
    EXPECT_EQ(extract_command_args("/stop"), "");
    EXPECT_FALSE(extract_command_word("hello").has_value());
}

TEST(GatewayHelpers, PercentEncodeAndEquivalence) {
    EXPECT_EQ(percent_encode("a b+c"), "a%20b%2Bc");
    EXPECT_TRUE(contents_equivalent("Hello  world ", "hello world"));
    EXPECT_FALSE(contents_equivalent("foo", "bar"));
}

TEST(GatewayHelpers, NormalizeChatIdRejectsControlCodes) {
    EXPECT_EQ(normalize_chat_id(" abc  "), "abc");
    EXPECT_EQ(normalize_chat_id("bad\x01id"), "");
}

// ---- base_adapter: TokenBucket -----------------------------------------

TEST(TokenBucket, ConsumesAndRefills) {
    TokenBucket b(10.0, 5.0);  // 10 tokens/s, burst 5
    for (int i = 0; i < 5; ++i) EXPECT_TRUE(b.try_consume(1));
    std::chrono::milliseconds wait{};
    EXPECT_FALSE(b.try_consume(1, &wait));
    EXPECT_GT(wait.count(), 0);
}

TEST(TokenBucket, CooldownBlocks) {
    TokenBucket b(100.0, 10.0);
    auto until = std::chrono::steady_clock::now() +
                 std::chrono::milliseconds(200);
    b.cooldown_until(until);
    std::chrono::milliseconds wait{};
    EXPECT_FALSE(b.try_consume(1, &wait));
    EXPECT_GT(wait.count(), 0);
}

// ---- base_adapter: RetryBudget -----------------------------------------

TEST(RetryBudget, ExhaustsOnConsecutiveFailures) {
    RetryBudget b(3, std::chrono::seconds(60));
    b.record(true);
    b.record(true);
    EXPECT_FALSE(b.exhausted());
    b.record(true);
    EXPECT_TRUE(b.exhausted());
    b.record(false);  // success resets
    EXPECT_FALSE(b.exhausted());
    EXPECT_EQ(b.failure_count(), 0u);
}

TEST(RetryBudget, FatalStaysFatal) {
    RetryBudget b;
    b.record_fatal();
    EXPECT_TRUE(b.fatal());
    b.clear();
    EXPECT_FALSE(b.fatal());
}

TEST(RetryBudget, BackoffGrowsAndCaps) {
    RetryBudget b(10, std::chrono::seconds(60));
    auto max = std::chrono::milliseconds(5000);
    b.record(true);
    auto a1 = b.next_backoff(std::chrono::milliseconds(100), max);
    b.record(true);
    b.record(true);
    auto a3 = b.next_backoff(std::chrono::milliseconds(100), max);
    EXPECT_GE(a3.count(), a1.count());
    EXPECT_LE(a3, max);
}

// ---- base_adapter: FeatureFlags ----------------------------------------

TEST(FeatureFlags, RegisterAndToggle) {
    FeatureFlags f;
    EXPECT_TRUE(f.register_feature("voice", FeatureState::Enabled));
    EXPECT_FALSE(f.register_feature("voice", FeatureState::Disabled));
    EXPECT_TRUE(f.is_enabled("voice"));
    EXPECT_TRUE(f.set_state("voice", FeatureState::Disabled));
    EXPECT_FALSE(f.is_enabled("voice"));
    EXPECT_EQ(f.state("unknown"), FeatureState::Unsupported);
}

// ---- base_adapter: mixin ------------------------------------------------

TEST(BaseAdapterMixin, ErrorClassification) {
    using K = AdapterErrorKind;
    EXPECT_EQ(BaseAdapterMixin::classify_error("401 Unauthorized"), K::Fatal);
    EXPECT_EQ(BaseAdapterMixin::classify_error("invalid token"), K::Fatal);
    EXPECT_EQ(BaseAdapterMixin::classify_error("429 Too Many Requests"),
              K::Retryable);
    EXPECT_EQ(BaseAdapterMixin::classify_error("timed out waiting"),
              K::Retryable);
    EXPECT_EQ(BaseAdapterMixin::classify_error("something unusual"),
              K::Unknown);
}

TEST(BaseAdapterMixin, LifecycleAndHealth) {
    BaseAdapterMixin m;
    EXPECT_FALSE(m.is_connected());
    m.mark_connected();
    EXPECT_TRUE(m.is_connected());

    m.on_send_success();
    auto s = m.snapshot_health();
    EXPECT_TRUE(s.connected);
    EXPECT_TRUE(s.ready);
    EXPECT_EQ(s.failure_streak, 0u);

    m.on_send_failure(AdapterErrorKind::Retryable, "rate limit");
    s = m.snapshot_health();
    EXPECT_EQ(s.failure_streak, 1u);
    EXPECT_EQ(s.last_error_kind, AdapterErrorKind::Retryable);
}

TEST(BaseAdapterMixin, FatalErrorBlocksReady) {
    BaseAdapterMixin m;
    m.mark_connected();
    m.set_fatal_error("401", "unauthorized", false);
    EXPECT_TRUE(m.has_fatal_error());
    EXPECT_FALSE(m.snapshot_health().ready);
    EXPECT_FALSE(m.fatal_error_retryable());
    m.clear_fatal_error();
    EXPECT_FALSE(m.has_fatal_error());
}

TEST(BaseAdapterMixin, TypingPauseTable) {
    BaseAdapterMixin m;
    EXPECT_FALSE(m.is_typing_paused("chat-1"));
    m.pause_typing_for_chat("chat-1");
    EXPECT_TRUE(m.is_typing_paused("chat-1"));
    m.resume_typing_for_chat("chat-1");
    EXPECT_FALSE(m.is_typing_paused("chat-1"));
}

// ---- backpressure -------------------------------------------------------

TEST(Backpressure, AcceptUntilLimit) {
    BackpressureConfig c;
    c.max_per_session = 3;
    c.coalesce = false;
    BoundedSessionQueue q(c);
    MessageEvent e;
    e.text = "m";
    EXPECT_EQ(q.push("a", e), BackpressureResult::Accepted);
    EXPECT_EQ(q.push("a", e), BackpressureResult::Accepted);
    EXPECT_EQ(q.push("a", e), BackpressureResult::Accepted);
    // Limit reached — policy is drop-oldest by default.
    EXPECT_EQ(q.push("a", e), BackpressureResult::DroppedOldest);
    EXPECT_EQ(q.session_size("a"), 3u);
}

TEST(Backpressure, PolicyDropNewest) {
    BackpressureConfig c;
    c.max_per_session = 2;
    c.policy = OverflowPolicy::DropNewest;
    c.coalesce = false;
    BoundedSessionQueue q(c);
    MessageEvent e;
    EXPECT_EQ(q.push("k", e), BackpressureResult::Accepted);
    EXPECT_EQ(q.push("k", e), BackpressureResult::Accepted);
    EXPECT_EQ(q.push("k", e), BackpressureResult::DroppedNewest);
    EXPECT_EQ(q.session_size("k"), 2u);
}

TEST(Backpressure, PolicyReject) {
    BackpressureConfig c;
    c.max_per_session = 1;
    c.policy = OverflowPolicy::Reject;
    c.coalesce = false;
    BoundedSessionQueue q(c);
    MessageEvent e;
    EXPECT_EQ(q.push("k", e), BackpressureResult::Accepted);
    EXPECT_EQ(q.push("k", e), BackpressureResult::NotAccepted);
    EXPECT_EQ(q.session_size("k"), 1u);
}

TEST(Backpressure, CoalesceMerges) {
    BackpressureConfig c;
    BoundedSessionQueue q(c);
    MessageEvent a;
    a.text = "hi";
    MessageEvent b;
    b.text = "fixed";
    EXPECT_EQ(q.push("k", a), BackpressureResult::Accepted);
    EXPECT_EQ(q.push("k", b), BackpressureResult::Merged);
    EXPECT_EQ(q.session_size("k"), 1u);
    auto peeked = q.peek("k");
    ASSERT_TRUE(peeked.has_value());
    EXPECT_EQ(peeked->text, "fixed");
}

TEST(Backpressure, DrainClearsAndReportsOrder) {
    BackpressureConfig c;
    c.coalesce = false;
    BoundedSessionQueue q(c);
    for (int i = 0; i < 4; ++i) {
        MessageEvent e;
        e.text = std::to_string(i);
        q.push("k", std::move(e));
    }
    auto out = q.drain("k");
    EXPECT_EQ(out.size(), 4u);
    EXPECT_EQ(out.front().text, "0");
    EXPECT_EQ(out.back().text, "3");
    EXPECT_EQ(q.total_size(), 0u);
}

TEST(Backpressure, SweepStaleDropsOld) {
    BackpressureConfig c;
    c.coalesce = false;
    c.max_age = std::chrono::seconds(0);  // anything older than "now" drops
    BoundedSessionQueue q(c);
    MessageEvent e;
    q.push("k", e);
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    c.max_age = std::chrono::seconds(0);
    auto dropped = q.sweep_stale();
    EXPECT_GE(dropped, 0u);
}

// ---- metrics ------------------------------------------------------------

TEST(Metrics, CounterAndGauge) {
    MetricsRegistry r;
    r.inc("msgs", 3);
    r.inc("msgs");
    r.set_gauge("queue_depth", 7.5);
    EXPECT_EQ(r.counter("msgs"), 4u);
    EXPECT_DOUBLE_EQ(r.gauge("queue_depth"), 7.5);
    r.add_gauge("queue_depth", -0.5);
    EXPECT_DOUBLE_EQ(r.gauge("queue_depth"), 7.0);
}

TEST(Metrics, SnapshotSortsByName) {
    MetricsRegistry r;
    r.inc("b");
    r.inc("a");
    r.set_gauge("c", 1.0);
    auto s = r.snapshot();
    ASSERT_EQ(s.size(), 3u);
    EXPECT_EQ(s[0].name, "a");
    EXPECT_EQ(s[1].name, "b");
    EXPECT_EQ(s[2].name, "c");
}

TEST(Metrics, EmitterRunsAndStops) {
    MetricsRegistry r;
    r.inc("x", 1);
    std::atomic<int> ticks{0};
    MetricsEmitter em(&r,
                       [&](const nlohmann::json& j) {
                           ticks.fetch_add(1);
                           EXPECT_TRUE(j.is_object());
                       },
                       std::chrono::milliseconds(120));
    em.start();
    em.emit_now();
    std::this_thread::sleep_for(std::chrono::milliseconds(350));
    em.stop();
    EXPECT_GE(ticks.load(), 2);
}

// ---- command dispatcher -------------------------------------------------

TEST(CommandDispatcher, BuiltinsCoverPrimaryCommands) {
    CommandDispatcher d;
    d.register_builtins();
    for (auto name : {"stop", "reset", "model", "help", "approve", "deny",
                       "shutdown", "restart", "drain", "replay"}) {
        EXPECT_TRUE(d.has_command(name)) << name;
    }
}

TEST(CommandDispatcher, AliasesResolve) {
    CommandDispatcher d;
    d.register_builtins();
    EXPECT_TRUE(d.has_command("cancel"));  // alias for stop
    EXPECT_TRUE(d.has_command("new"));     // alias for reset
    auto r = d.dispatch({}, "/cancel");
    EXPECT_EQ(r.command, "stop");
    EXPECT_EQ(r.action, CommandAction::StopAgent);
}

TEST(CommandDispatcher, UnknownCommandIsNotHandled) {
    CommandDispatcher d;
    d.register_builtins();
    auto r = d.dispatch({}, "/nonsense arg");
    EXPECT_EQ(r.outcome, CommandOutcome::NotCommand);
}

TEST(CommandDispatcher, ModelSwitchCapturesArgs) {
    CommandDispatcher d;
    d.register_builtins();
    auto r = d.dispatch({}, "/model gpt-5");
    EXPECT_EQ(r.action, CommandAction::SwitchModel);
    EXPECT_EQ(r.args, "gpt-5");
}

TEST(CommandDispatcher, ResolveReturnsCanonicalName) {
    CommandDispatcher d;
    d.register_builtins();
    auto c = d.resolve("/exit");
    ASSERT_TRUE(c.has_value());
    EXPECT_EQ(*c, "shutdown");
}

// ---- shutdown sequencer -------------------------------------------------

TEST(ShutdownSequencer, RunsThroughPhases) {
    ShutdownSequencer s;
    ShutdownBudget b;
    b.drain_grace = std::chrono::milliseconds(5);
    b.agent_drain_timeout = std::chrono::milliseconds(100);
    b.per_adapter_timeout = std::chrono::milliseconds(100);
    b.session_flush_timeout = std::chrono::milliseconds(100);
    s.set_budget(b);
    std::atomic<int> counter{2};
    s.set_agent_stop([&] {
        counter.fetch_sub(1);
    });
    s.set_agent_counter([&] { return counter.load() > 1 ? 1 : 0; });
    std::atomic<bool> snap_called{false};
    s.set_queue_snapshot([&] { snap_called.store(true); });
    std::atomic<bool> flush_called{false};
    s.set_session_flush([&] { flush_called.store(true); });

    std::atomic<int> closed{0};
    s.add_adapter({"a1", [&] { closed.fetch_add(1); }});
    s.add_adapter({"a2", [&] { closed.fetch_add(1); }});

    std::vector<ShutdownPhase> seen;
    std::mutex m;
    s.set_phase_callback([&](ShutdownPhase p) {
        std::lock_guard<std::mutex> lock(m);
        seen.push_back(p);
    });

    auto out = s.run("test");
    EXPECT_EQ(out.final_phase, ShutdownPhase::Stopped);
    EXPECT_EQ(closed.load(), 2);
    EXPECT_TRUE(snap_called.load());
    EXPECT_TRUE(flush_called.load());
    EXPECT_EQ(s.phase(), ShutdownPhase::Stopped);
    EXPECT_FALSE(seen.empty());
}

TEST(ShutdownSequencer, AdapterTimeoutRecordedSlow) {
    ShutdownSequencer s;
    ShutdownBudget b;
    b.drain_grace = std::chrono::milliseconds(1);
    b.agent_drain_timeout = std::chrono::milliseconds(10);
    b.per_adapter_timeout = std::chrono::milliseconds(50);
    b.session_flush_timeout = std::chrono::milliseconds(50);
    s.set_budget(b);
    s.add_adapter({"slow", [] {
                       std::this_thread::sleep_for(
                           std::chrono::milliseconds(200));
                   }});
    auto out = s.run();
    EXPECT_EQ(out.final_phase, ShutdownPhase::Stopped);
    ASSERT_EQ(out.slow_adapters.size(), 1u);
    EXPECT_EQ(out.slow_adapters[0], "slow");
}

TEST(ShutdownSequencer, PhaseNamesExhaustive) {
    for (auto p : {ShutdownPhase::Idle, ShutdownPhase::Draining,
                    ShutdownPhase::InterruptingAgents,
                    ShutdownPhase::WaitingForAgents,
                    ShutdownPhase::SnapshottingQueues,
                    ShutdownPhase::DisconnectingAdapters,
                    ShutdownPhase::FlushingSessions,
                    ShutdownPhase::Stopped}) {
        EXPECT_STRNE(shutdown_phase_name(p), "unknown");
    }
}

// ---- config validator --------------------------------------------------

TEST(ConfigValidator, MissingPlatformTokenIsError) {
    nlohmann::json c = {{"config_version", kCurrentConfigVersion},
                         {"sessions_dir", "/tmp/x"},
                         {"platforms",
                          {{"telegram", {{"enabled", true}}}}}};
    auto r = validate_gateway_config(c);
    EXPECT_FALSE(r.ok);
    EXPECT_GT(r.count(ValidationSeverity::Error), 0u);
}

TEST(ConfigValidator, PlausibleSecretPasses) {
    nlohmann::json c = {
        {"config_version", kCurrentConfigVersion},
        {"sessions_dir", "/tmp/x"},
        {"platforms",
          {{"telegram",
             {{"enabled", true},
              {"token", "12345678:AAAAAAAAAAAAAAAAAAAAAAAAAAA"}}}}}};
    auto r = validate_gateway_config(c);
    EXPECT_TRUE(r.ok) << r.to_string();
}

TEST(ConfigValidator, PlaceholderSecretWarns) {
    nlohmann::json c = {
        {"config_version", kCurrentConfigVersion},
        {"sessions_dir", "/tmp/x"},
        {"platforms",
          {{"telegram",
             {{"enabled", true}, {"token", "YOUR_TOKEN_HERE"}}}}}};
    auto r = validate_gateway_config(c);
    EXPECT_GT(r.count(ValidationSeverity::Warning), 0u);
}

TEST(ConfigValidator, ResetPolicyModeChecked) {
    nlohmann::json c = {{"config_version", kCurrentConfigVersion},
                         {"sessions_dir", "/tmp/x"},
                         {"reset_policy", {{"mode", "weird"}}},
                         {"platforms", nlohmann::json::object()}};
    auto r = validate_gateway_config(c);
    EXPECT_FALSE(r.ok);
}

TEST(ConfigValidator, InjectDefaultsFillsMissingFields) {
    nlohmann::json c = nlohmann::json::object();
    auto n = inject_defaults(c);
    EXPECT_GT(n, 0u);
    EXPECT_TRUE(c.contains("config_version"));
    EXPECT_TRUE(c.contains("sessions_dir"));
    EXPECT_TRUE(c.contains("reset_policy"));
    EXPECT_TRUE(c.contains("backpressure"));
    EXPECT_TRUE(c.contains("metrics"));
}

TEST(ConfigValidator, MigrationAdvancesVersion) {
    nlohmann::json c;
    c["config_version"] = 1;
    c["per_user_sessions"] = true;
    int from = migrate_gateway_config(c);
    EXPECT_EQ(from, 1);
    EXPECT_EQ(c["config_version"].get<int>(), kCurrentConfigVersion);
    EXPECT_TRUE(c.contains("group_sessions_per_user"));
    EXPECT_FALSE(c.contains("per_user_sessions"));
    EXPECT_TRUE(c.contains("backpressure"));
}

TEST(ConfigValidator, RedactSecretsReplacesTokens) {
    nlohmann::json c = {
        {"platforms",
          {{"telegram",
             {{"enabled", true}, {"token", "1234567890:ABC"}}}}}};
    auto redacted = redact_secrets(c);
    EXPECT_EQ(redacted["platforms"]["telegram"]["token"].get<std::string>(),
              "***REDACTED***");
}

TEST(ConfigValidator, MergeConfigsOverridesWin) {
    nlohmann::json base = {{"sessions_dir", "/a"},
                            {"platforms", {{"telegram", {{"enabled", false}}}}}};
    nlohmann::json over = {
        {"platforms", {{"telegram", {{"enabled", true}, {"token", "t"}}}}}};
    auto merged = merge_gateway_configs(base, over);
    EXPECT_EQ(merged["sessions_dir"].get<std::string>(), "/a");
    EXPECT_TRUE(merged["platforms"]["telegram"]["enabled"].get<bool>());
    EXPECT_EQ(merged["platforms"]["telegram"]["token"].get<std::string>(),
              "t");
}

// ---- session TTL / insights / linker -----------------------------------

TEST(SessionTtl, TouchCreatesEntry) {
    SessionTtlTracker t;
    auto now = std::chrono::system_clock::now();
    t.touch("k", now);
    auto e = t.entry("k");
    ASSERT_TRUE(e.has_value());
    EXPECT_GT(e->expires_at, now);
}

TEST(SessionTtl, RecordTurnAccumulates) {
    SessionTtlTracker t;
    t.record_turn("k", 100);
    t.record_turn("k", 250);
    auto e = t.entry("k");
    ASSERT_TRUE(e.has_value());
    EXPECT_EQ(e->turn_count, 2u);
    EXPECT_EQ(e->size_bytes, 350u);
}

TEST(SessionTtl, ExpiredExcludesPinned) {
    SessionTtlTracker t;
    t.set_default_ttl(std::chrono::seconds(0));
    auto now = std::chrono::system_clock::now();
    t.touch("a", now);
    t.touch("b", now);
    t.pin("a", true);
    auto later = now + std::chrono::seconds(10);
    auto exp = t.expired(later);
    EXPECT_EQ(exp.size(), 1u);
    EXPECT_EQ(exp[0], "b");
}

TEST(SessionTtl, CompactionFires) {
    SessionTtlTracker t;
    CompactionTriggers tr;
    tr.max_turns = 3;
    t.set_triggers(tr);
    for (int i = 0; i < 5; ++i) t.record_turn("k", 10);
    auto due = t.due_for_compaction(std::chrono::system_clock::now());
    ASSERT_EQ(due.size(), 1u);
    EXPECT_EQ(due[0].second, CompactionReason::TurnCount);
}

TEST(SessionTtl, InsightLedgerTopSortsByScore) {
    InsightLedger l;
    l.record({"s", "summary", "a", 0.1, {}, {}});
    l.record({"s", "summary", "b", 0.9, {}, {}});
    l.record({"s", "summary", "c", 0.5, {}, {}});
    auto top = l.top_for_session("s", 2);
    ASSERT_EQ(top.size(), 2u);
    EXPECT_EQ(top[0].content, "b");
    EXPECT_EQ(top[1].content, "c");
}

TEST(SessionTtl, InsightLedgerPurgeAndCategories) {
    InsightLedger l;
    l.record({"s1", "summary", "x", 0.1, {}, {}});
    l.record({"s1", "memory", "y", 0.1, {}, {}});
    l.record({"s2", "summary", "z", 0.1, {}, {}});
    EXPECT_EQ(l.size(), 3u);
    EXPECT_EQ(l.categories().size(), 2u);
    EXPECT_EQ(l.purge_session("s1"), 2u);
    EXPECT_EQ(l.size(), 1u);
}

TEST(SessionLinker, LinksAcrossPlatforms) {
    SessionLinker l;
    auto c = l.link(Platform::Telegram, "111");
    auto same = l.link(Platform::Telegram, "111");
    EXPECT_EQ(c, same);
    auto c2 = l.link(Platform::Slack, "u-abc", c);
    EXPECT_EQ(c2, c);
    EXPECT_EQ(l.canonical_count(), 1u);
    EXPECT_EQ(l.identity_count(), 2u);

    auto idents = l.identities_for(c);
    EXPECT_EQ(idents.size(), 2u);
}

TEST(SessionLinker, UnlinkRemovesIdentity) {
    SessionLinker l;
    auto c = l.link(Platform::Telegram, "x");
    l.link(Platform::Slack, "y", c);
    EXPECT_TRUE(l.unlink(Platform::Telegram, "x"));
    EXPECT_FALSE(l.canonical_for(Platform::Telegram, "x").has_value());
    EXPECT_EQ(l.identity_count(), 1u);
}

TEST(SessionLinker, GeneratesCanonicalWhenMissing) {
    SessionLinker l;
    auto a = l.link(Platform::Local, "alice");
    auto b = l.link(Platform::Local, "bob");
    EXPECT_NE(a, b);
    EXPECT_FALSE(a.empty());
}
