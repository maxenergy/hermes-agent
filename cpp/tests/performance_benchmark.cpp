// Performance benchmarks for key Hermes operations.
// Uses simple timing with GoogleTest assertions on latency bounds.

#include "hermes/cron/cron_parser.hpp"
#include "hermes/state/session_db.hpp"
#include "hermes/tools/registry.hpp"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <string>

using json = nlohmann::json;

namespace {

struct TmpDir {
    std::filesystem::path path;
    TmpDir() {
        auto base = std::filesystem::temp_directory_path() / "hermes_bench";
        path = base / std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count());
        std::filesystem::create_directories(path);
    }
    ~TmpDir() {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }
};

template <typename Fn>
double measure_us(Fn&& fn) {
    auto start = std::chrono::high_resolution_clock::now();
    fn();
    auto end = std::chrono::high_resolution_clock::now();
    return std::chrono::duration<double, std::micro>(end - start).count();
}

template <typename Fn>
double measure_ms(Fn&& fn) {
    return measure_us(std::forward<Fn>(fn)) / 1000.0;
}

}  // namespace

// ---- Benchmark 1: ToolRegistry dispatch latency ----

TEST(Benchmark, ToolRegistryDispatch1000) {
    auto& reg = hermes::tools::ToolRegistry::instance();
    reg.clear();

    // Register 50 tools.
    for (int i = 0; i < 50; ++i) {
        hermes::tools::ToolEntry entry;
        entry.name = "bench_tool_" + std::to_string(i);
        entry.toolset = "bench";
        entry.schema = json{
            {"name", entry.name},
            {"description", "Benchmark tool " + std::to_string(i)},
            {"parameters", {{"type", "object"}, {"properties", json::object()}}},
        };
        entry.handler = [](const json&,
                           const hermes::tools::ToolContext&) -> std::string {
            return R"({"ok":true})";
        };
        reg.register_tool(entry);
    }

    hermes::tools::ToolContext ctx;
    double total_us = 0;
    const int iterations = 1000;

    for (int i = 0; i < iterations; ++i) {
        auto tool_name = "bench_tool_" + std::to_string(i % 50);
        total_us += measure_us([&] {
            reg.dispatch(tool_name, json::object(), ctx);
        });
    }

    double avg_us = total_us / iterations;
    std::printf("  ToolRegistry dispatch avg: %.1f us (%d iterations)\n",
                avg_us, iterations);
    EXPECT_LT(avg_us, 100.0) << "Average dispatch latency exceeds 100us";

    reg.clear();
}

// ---- Benchmark 2: SessionDB write throughput ----

TEST(Benchmark, SessionDBWrite1000) {
    TmpDir tmp;
    auto db_path = tmp.path / "bench.db";
    hermes::state::SessionDB db(db_path);

    auto sid = db.create_session("benchmark", "gpt-4o", json{});

    const int count = 1000;
    double total_ms = measure_ms([&] {
        for (int i = 0; i < count; ++i) {
            hermes::state::MessageRow msg;
            msg.session_id = sid;
            msg.turn_index = i;
            msg.role = (i % 2 == 0) ? "user" : "assistant";
            msg.content = "Benchmark message " + std::to_string(i) +
                          " with some padding content for realism.";
            msg.created_at = std::chrono::system_clock::now();
            db.save_message(msg);
        }
    });

    double writes_per_sec = (count * 1000.0) / total_ms;
    std::printf("  SessionDB writes: %.0f/sec (%.1f ms for %d writes)\n",
                writes_per_sec, total_ms, count);
    EXPECT_GT(writes_per_sec, 500.0)
        << "Write throughput below 500/sec: " << writes_per_sec;
}

// ---- Benchmark 3: FTS search latency ----

TEST(Benchmark, FtsSearch) {
    TmpDir tmp;
    auto db_path = tmp.path / "fts_bench.db";
    hermes::state::SessionDB db(db_path);

    // Insert 100 sessions with diverse content.
    for (int i = 0; i < 100; ++i) {
        auto sid = db.create_session("fts_bench", "gpt-4o", json{});
        hermes::state::MessageRow msg;
        msg.session_id = sid;
        msg.turn_index = 0;
        msg.role = "user";
        msg.content = "Session " + std::to_string(i) +
                      " discusses quantum computing, machine learning, "
                      "neural networks, compiler design, and operating systems. "
                      "Unique marker: xyzzy_" + std::to_string(i);
        msg.created_at = std::chrono::system_clock::now();
        db.save_message(msg);
    }

    // Search for various terms.
    const int searches = 20;
    double total_ms = 0;
    for (int i = 0; i < searches; ++i) {
        total_ms += measure_ms([&] {
            auto hits = db.fts_search("quantum computing");
            EXPECT_GT(hits.size(), 0u);
        });
    }

    double avg_ms = total_ms / searches;
    std::printf("  FTS search avg: %.2f ms (%d searches over 100 sessions)\n",
                avg_ms, searches);
    EXPECT_LT(avg_ms, 50.0) << "FTS search exceeds 50ms average";
}

// ---- Benchmark 4: Cron parser next_fire ----

TEST(Benchmark, CronParserNextFire1000) {
    auto expr = hermes::cron::parse("*/5 * * * *");
    auto now = std::chrono::system_clock::now();

    const int iterations = 1000;
    double total_ms = measure_ms([&] {
        for (int i = 0; i < iterations; ++i) {
            auto nf = hermes::cron::next_fire(expr, now);
            // Prevent optimization from eliding the call.
            EXPECT_GT(nf.time_since_epoch().count(), 0);
        }
    });

    std::printf("  CronParser next_fire: %.3f ms total for %d iterations\n",
                total_ms, iterations);
    EXPECT_LT(total_ms, 10.0)
        << "1000 next_fire calls took " << total_ms << " ms (> 10ms)";
}

// ---- Benchmark 5: Startup latency (config + SessionDB + tool discovery) ----

TEST(Benchmark, StartupLatency) {
    TmpDir tmp;
    auto db_path = tmp.path / "startup.db";

    double total_ms = measure_ms([&] {
        // Simulate startup: create SessionDB + register tools.
        hermes::state::SessionDB db(db_path);
        auto& reg = hermes::tools::ToolRegistry::instance();
        reg.clear();

        // Register a representative set of tools.
        for (int i = 0; i < 49; ++i) {
            hermes::tools::ToolEntry entry;
            entry.name = "startup_tool_" + std::to_string(i);
            entry.toolset = "default";
            entry.schema = json{
                {"name", entry.name},
                {"description", "Startup test tool"},
                {"parameters", {{"type", "object"}}},
            };
            entry.handler = [](const json&,
                               const hermes::tools::ToolContext&) -> std::string {
                return R"({"ok":true})";
            };
            reg.register_tool(entry);
        }

        EXPECT_EQ(reg.size(), 49u);
        reg.clear();
    });

    std::printf("  Startup latency: %.1f ms (SessionDB + 49 tools)\n", total_ms);
    EXPECT_LT(total_ms, 500.0)
        << "Startup exceeded 500ms: " << total_ms << " ms";
}

// ---- Benchmark 6: Session list + message retrieval ----

TEST(Benchmark, SessionListAndRetrieve) {
    TmpDir tmp;
    auto db_path = tmp.path / "list_bench.db";
    hermes::state::SessionDB db(db_path);

    // Create 50 sessions with 10 messages each.
    for (int s = 0; s < 50; ++s) {
        auto sid = db.create_session("list_bench", "gpt-4o", json{});
        for (int m = 0; m < 10; ++m) {
            hermes::state::MessageRow msg;
            msg.session_id = sid;
            msg.turn_index = m;
            msg.role = (m % 2 == 0) ? "user" : "assistant";
            msg.content = "Message " + std::to_string(m) + " in session " +
                          std::to_string(s);
            msg.created_at = std::chrono::system_clock::now();
            db.save_message(msg);
        }
    }

    double list_ms = measure_ms([&] {
        auto sessions = db.list_sessions(50);
        EXPECT_EQ(sessions.size(), 50u);
    });

    auto sessions = db.list_sessions(50);
    double retrieve_ms = measure_ms([&] {
        for (auto& session : sessions) {
            auto msgs = db.get_messages(session.id);
            EXPECT_EQ(msgs.size(), 10u);
        }
    });

    std::printf("  List 50 sessions: %.2f ms\n", list_ms);
    std::printf("  Retrieve messages (50 sessions x 10 msgs): %.2f ms\n",
                retrieve_ms);
    EXPECT_LT(list_ms, 50.0);
    EXPECT_LT(retrieve_ms, 200.0);
}
