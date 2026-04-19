// Tests for the bounded LRU + idle-TTL agent cache on SessionManager.
//
// Ports the behavioural guarantees of Python gateway/run.py
// _enforce_agent_cache_cap / _sweep_idle_cached_agents (upstream
// 8d7b7feb).  We don't construct real AIAgent instances in these tests
// — that requires LLM + SessionDB + context-engine wiring — so we use
// the shared_ptr aliasing trick to get distinct AIAgent* identities
// backed by small POD owners.  The cache never dereferences the agent
// pointer, so this is safe.
#include <gtest/gtest.h>

#include <hermes/agent/ai_agent.hpp>
#include <hermes/gateway/session_manager.hpp>

#include <atomic>
#include <chrono>
#include <memory>
#include <thread>

namespace hg = hermes::gateway;

namespace {

// Produce a shared_ptr<AIAgent> with a unique identity that can be
// stored/compared but never dereferenced.  Backed by a heap int owner
// for RAII; we cast its address to AIAgent* via the shared_ptr aliasing
// constructor.
std::shared_ptr<hermes::agent::AIAgent> fake_agent() {
    auto owner = std::make_shared<int>(0);
    auto* raw = reinterpret_cast<hermes::agent::AIAgent*>(owner.get());
    return std::shared_ptr<hermes::agent::AIAgent>(owner, raw);
}

}  // namespace

TEST(AgentCacheLRU, CapEvictsLeastRecentlyUsed) {
    hg::SessionManager mgr;
    mgr.set_agent_cache_max_size(3);
    int factory_calls = 0;
    mgr.set_agent_factory([&](const std::string&) {
        ++factory_calls;
        return fake_agent();
    });

    auto a = mgr.get_or_create_agent("A");  // cache: [A]
    auto b = mgr.get_or_create_agent("B");  // cache: [A, B]
    auto c = mgr.get_or_create_agent("C");  // cache: [A, B, C]
    EXPECT_EQ(mgr.agent_cache_size(), 3u);
    EXPECT_EQ(factory_calls, 3);

    // Insert D — A is LRU, should be evicted.
    auto d = mgr.get_or_create_agent("D");
    EXPECT_EQ(mgr.agent_cache_size(), 3u);

    // Accessing A again should rebuild via factory (cache miss).
    auto a2 = mgr.get_or_create_agent("A");
    EXPECT_EQ(factory_calls, 5);  // D then A rebuild
    EXPECT_NE(a.get(), a2.get());
}

TEST(AgentCacheLRU, CacheHitRefreshesLruOrder) {
    hg::SessionManager mgr;
    mgr.set_agent_cache_max_size(3);
    mgr.set_agent_factory([](const std::string&) { return fake_agent(); });

    mgr.get_or_create_agent("A");
    mgr.get_or_create_agent("B");
    mgr.get_or_create_agent("C");

    // Touch A -> now MRU.  LRU becomes B.
    mgr.get_or_create_agent("A");

    // Insert D -> B evicted (LRU).
    mgr.get_or_create_agent("D");

    // A should still be cached (factory miss), B should be a factory hit.
    int factory_calls = 0;
    mgr.set_agent_factory([&](const std::string&) {
        ++factory_calls;
        return fake_agent();
    });
    mgr.get_or_create_agent("A");
    EXPECT_EQ(factory_calls, 0);
    mgr.get_or_create_agent("B");
    EXPECT_EQ(factory_calls, 1);
}

TEST(AgentCacheLRU, ReleaseCallbackInvokedOnEviction) {
    hg::SessionManager mgr;
    mgr.set_agent_cache_max_size(2);

    std::atomic<int> release_calls{0};
    mgr.set_agent_release(
        [&](std::shared_ptr<hermes::agent::AIAgent>) { ++release_calls; });
    mgr.set_agent_factory([](const std::string&) { return fake_agent(); });

    mgr.get_or_create_agent("A");
    mgr.get_or_create_agent("B");
    mgr.get_or_create_agent("C");  // Evicts A.
    EXPECT_EQ(release_calls.load(), 1);

    mgr.get_or_create_agent("D");  // Evicts B.
    EXPECT_EQ(release_calls.load(), 2);
}

TEST(AgentCacheLRU, MidTurnAgentNotEvictedByCap) {
    // Upstream 8d7b7feb: when a mid-turn agent falls into the LRU
    // excess window, it is SKIPPED without compensating — we do NOT
    // substitute a newer entry.  The cache may therefore stay over
    // cap until the active turn finishes.
    hg::SessionManager mgr;
    mgr.set_agent_cache_max_size(2);
    mgr.set_agent_factory([](const std::string&) { return fake_agent(); });

    auto a = mgr.get_or_create_agent("A");
    auto b = mgr.get_or_create_agent("B");

    // Mark A (LRU) as actively running a turn.
    mgr.mark_running("A", a);

    std::atomic<int> evictions{0};
    mgr.set_agent_release(
        [&](std::shared_ptr<hermes::agent::AIAgent>) { ++evictions; });

    // Insert C.  Excess=1, A is the only candidate and is mid-turn, so
    // A stays AND B stays — no compensation.
    mgr.get_or_create_agent("C");
    EXPECT_EQ(evictions.load(), 0);
    EXPECT_EQ(mgr.agent_cache_size(), 3u);

    // Both A and B are still cached — no factory re-entry.
    int factory_calls = 0;
    mgr.set_agent_factory([&](const std::string&) {
        ++factory_calls;
        return fake_agent();
    });
    mgr.get_or_create_agent("A");
    mgr.get_or_create_agent("B");
    EXPECT_EQ(factory_calls, 0);

    // Once A finishes its turn, the NEXT insert re-checks the cap and
    // evicts any non-mid-turn LRU entries that fall in the excess
    // window.  Cache had {A, B, C} over cap=2; inserting D makes it 4,
    // excess=2, and with A no longer mid-turn both A and B are evicted.
    mgr.mark_finished("A");
    mgr.get_or_create_agent("D");
    EXPECT_EQ(evictions.load(), 2);
    EXPECT_EQ(mgr.agent_cache_size(), 2u);
}

TEST(AgentCacheLRU, IdleTtlSweepEvictsOldAgents) {
    hg::SessionManager mgr;
    mgr.set_agent_cache_max_size(100);
    // TTL = 1s so we can actually test it.
    mgr.set_agent_cache_idle_ttl(std::chrono::seconds(1));
    mgr.set_agent_factory([](const std::string&) { return fake_agent(); });

    mgr.get_or_create_agent("idle_A");
    mgr.get_or_create_agent("idle_B");

    // No sweep should happen yet.
    EXPECT_EQ(mgr.sweep_idle_cached_agents(), 0u);

    // Sleep past the TTL.
    std::this_thread::sleep_for(std::chrono::milliseconds(1200));

    std::atomic<int> released{0};
    mgr.set_agent_release(
        [&](std::shared_ptr<hermes::agent::AIAgent>) { ++released; });

    EXPECT_EQ(mgr.sweep_idle_cached_agents(), 2u);
    EXPECT_EQ(released.load(), 2);
    EXPECT_EQ(mgr.agent_cache_size(), 0u);
}

TEST(AgentCacheLRU, IdleTtlSweepSkipsMidTurnAgents) {
    hg::SessionManager mgr;
    mgr.set_agent_cache_max_size(100);
    mgr.set_agent_cache_idle_ttl(std::chrono::seconds(1));
    mgr.set_agent_factory([](const std::string&) { return fake_agent(); });

    auto a = mgr.get_or_create_agent("active");
    auto b = mgr.get_or_create_agent("idle");

    mgr.mark_running("active", a);

    std::this_thread::sleep_for(std::chrono::milliseconds(1200));

    EXPECT_EQ(mgr.sweep_idle_cached_agents(), 1u);  // idle only

    // active still cached.
    int factory_calls = 0;
    mgr.set_agent_factory([&](const std::string&) {
        ++factory_calls;
        return fake_agent();
    });
    mgr.get_or_create_agent("active");
    EXPECT_EQ(factory_calls, 0);
}

TEST(AgentCacheLRU, DefaultCapAndTtlMatchUpstream) {
    hg::SessionManager mgr;
    EXPECT_EQ(mgr.agent_cache_max_size(),
               hg::SessionManager::kDefaultAgentCacheMax);
    EXPECT_EQ(mgr.agent_cache_idle_ttl(),
               hg::SessionManager::kDefaultAgentIdleTtl);
    EXPECT_EQ(hg::SessionManager::kDefaultAgentCacheMax, 128u);
    EXPECT_EQ(hg::SessionManager::kDefaultAgentIdleTtl,
               std::chrono::seconds(3600));
}

TEST(AgentCacheLRU, AllEntriesMidTurnLeavesCacheOverCap) {
    // Upstream comment: when every LRU-excess slot is mid-turn, the
    // cache stays transiently over cap and re-checks on the next
    // insert.  We assert the non-eviction (no tear-down) behaviour.
    hg::SessionManager mgr;
    mgr.set_agent_cache_max_size(2);
    mgr.set_agent_factory([](const std::string&) { return fake_agent(); });

    auto a = mgr.get_or_create_agent("A");
    auto b = mgr.get_or_create_agent("B");
    mgr.mark_running("A", a);
    mgr.mark_running("B", b);

    std::atomic<int> evictions{0};
    mgr.set_agent_release(
        [&](std::shared_ptr<hermes::agent::AIAgent>) { ++evictions; });

    // C would push the cache to 3 but both existing entries are mid-
    // turn.  No eviction is allowed.
    mgr.get_or_create_agent("C");
    EXPECT_EQ(evictions.load(), 0);
    EXPECT_EQ(mgr.agent_cache_size(), 3u);  // over cap, by design
}
