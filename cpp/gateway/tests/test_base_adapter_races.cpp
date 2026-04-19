// Regression tests for the two BaseAdapter race conditions fixed upstream
// by 3a635145 ("close pending-drain and late-arrival races in base adapter").
//
// R5 (duplicate-agent-spawn): _active_sessions entry must stay live across
// the pending-drain turn chain; only the interrupt Event is cleared, so
// concurrent inbound messages take the busy-handler (queue + interrupt)
// path instead of spawning a second background agent.
//
// R6 (message-dropped): before the finally block deletes _active_sessions,
// any late-arrival message in _pending_messages must be popped and a drain
// task spawned, otherwise the queued message is silently dropped.
#include <gtest/gtest.h>

#include <hermes/gateway/message_pipeline.hpp>
#include <hermes/gateway/session_manager.hpp>

#include <atomic>
#include <thread>
#include <vector>

namespace hg = hermes::gateway;

TEST(BaseAdapterRaces, ClearInterruptKeepsSessionLive) {
    hg::SessionManager mgr;
    ASSERT_TRUE(mgr.mark_pending("sess:A"));
    mgr.mark_running("sess:A", nullptr);
    mgr.set_interrupt_flag("sess:A");
    ASSERT_TRUE(mgr.is_interrupt_set("sess:A"));

    // Drain path: clear the interrupt but keep the entry live.
    EXPECT_TRUE(mgr.clear_interrupt_flag("sess:A"));
    EXPECT_FALSE(mgr.is_interrupt_set("sess:A"));
    EXPECT_TRUE(mgr.is_agent_running("sess:A"));
}

TEST(BaseAdapterRaces, DuplicateSpawnGuardHoldsDuringDrainChain) {
    // R5 regression: a concurrent inbound message during the typing_task
    // await must not pass the Level-1 guard and start a second agent.
    // The guard is "is_agent_running(session_key)".  When the drain
    // path only clears the interrupt (not the entry), is_agent_running
    // stays true and the busy-handler (queue) path wins.
    hg::SessionManager mgr;
    mgr.mark_running("sess:B", nullptr);
    mgr.set_interrupt_flag("sess:B");

    // Simulate the Python race: two threads.  Thread 1 is on the drain
    // path (clear_interrupt_flag); thread 2 is a concurrent inbound
    // message checking is_agent_running.
    std::atomic<bool> spawned_second{false};
    std::thread drain([&] {
        mgr.clear_interrupt_flag("sess:B");
    });
    std::thread inbound([&] {
        for (int i = 0; i < 1000; ++i) {
            if (!mgr.is_agent_running("sess:B")) {
                spawned_second.store(true);
                return;
            }
        }
    });
    drain.join();
    inbound.join();
    EXPECT_FALSE(spawned_second.load());
}

TEST(BaseAdapterRaces, LateArrivalDispatchedBeforeFinalize) {
    // R6 regression: a message lands in pending_messages during the
    // cleanup awaits.  finalize_with_late_drain must pop it and hand
    // it to drain_starter — NOT delete the session and drop it.
    hg::SessionManager mgr;
    hg::PendingQueue queue;

    mgr.mark_running("sess:C", nullptr);

    // Late-arrival: a message for sess:C is queued during cleanup.
    hg::MessageEvent ev;
    ev.text = "LATE";
    ev.message_type = "TEXT";
    ev.source.platform = hg::Platform::Telegram;
    ev.source.chat_id = "chat";
    queue.enqueue("sess:C", ev);

    std::string dispatched_key;
    std::string dispatched_text;
    auto drain = [&](const std::string& key, hg::MessageEvent event) {
        dispatched_key = key;
        dispatched_text = event.text;
    };

    bool spawned = mgr.finalize_with_late_drain("sess:C", &queue, drain);
    EXPECT_TRUE(spawned);
    EXPECT_EQ(dispatched_key, "sess:C");
    EXPECT_EQ(dispatched_text, "LATE");

    // Per upstream: when a late drain was dispatched we LEAVE the
    // session populated so the drain task's own lifecycle cleans it.
    EXPECT_TRUE(mgr.is_agent_running("sess:C"));
}

TEST(BaseAdapterRaces, NoLateArrivalReleasesSession) {
    hg::SessionManager mgr;
    hg::PendingQueue queue;

    mgr.mark_running("sess:D", nullptr);

    bool called = false;
    auto drain = [&](const std::string&, hg::MessageEvent) { called = true; };

    bool spawned = mgr.finalize_with_late_drain("sess:D", &queue, drain);
    EXPECT_FALSE(spawned);
    EXPECT_FALSE(called);
    // No late arrival — the entry must be released normally.
    EXPECT_FALSE(mgr.is_agent_running("sess:D"));
}

TEST(BaseAdapterRaces, ConcurrentInboundAfterDrainClearsNotDoubleSpawned) {
    // End-to-end: run the full mini-scenario.  T1 has finished a turn
    // and is on the drain path.  T2 is a new inbound message: it
    // should see the session busy and take the queue path, not spawn
    // its own agent.
    hg::SessionManager mgr;
    hg::PendingQueue queue;

    mgr.mark_running("sess:E", nullptr);
    mgr.set_interrupt_flag("sess:E");

    // T2 arrives during the drain.  It checks is_agent_running — which
    // must still be true since we only clear the Event, not the entry.
    std::atomic<int> busy_queue_hits{0};
    std::atomic<int> fresh_spawn_hits{0};

    auto drain_path = [&] {
        // Mirror upstream: clear the Event first, DO NOT erase.
        mgr.clear_interrupt_flag("sess:E");
    };
    auto inbound_path = [&] {
        for (int i = 0; i < 100; ++i) {
            if (mgr.is_agent_running("sess:E")) {
                ++busy_queue_hits;
                hg::MessageEvent ev;
                ev.text = "newer";
                queue.enqueue("sess:E", ev);
            } else {
                ++fresh_spawn_hits;
            }
        }
    };

    std::thread t1(drain_path);
    std::thread t2(inbound_path);
    t1.join();
    t2.join();

    EXPECT_EQ(fresh_spawn_hits.load(), 0);
    EXPECT_GT(busy_queue_hits.load(), 0);
}

TEST(BaseAdapterRaces, DrainStarterRaiseReleasesSession) {
    // Defensive: if drain_starter throws, we must not leak the session
    // row — the finalize path falls back to mark_finished.
    hg::SessionManager mgr;
    hg::PendingQueue queue;
    mgr.mark_running("sess:F", nullptr);
    hg::MessageEvent ev;
    ev.text = "late";
    queue.enqueue("sess:F", ev);

    auto throwing_drain = [](const std::string&, hg::MessageEvent) {
        throw std::runtime_error("dispatch failed");
    };

    bool spawned = mgr.finalize_with_late_drain("sess:F", &queue,
                                                  throwing_drain);
    EXPECT_FALSE(spawned);
    EXPECT_FALSE(mgr.is_agent_running("sess:F"));
}
