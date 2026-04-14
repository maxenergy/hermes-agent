// Session TTL + compaction + insight + cross-platform linking.
//
// Ports the Python facilities that layer on top of gateway/session.py
// (SessionStore):
//
//   - TTL bookkeeping for each session key
//   - Scheduled compaction triggers (size, age, idle)
//   - Insight recording hooks (tags, scores, summaries)
//   - Cross-platform session linking (e.g., Telegram user A <-> Slack
//     user A via shared canonical id)
//
// Kept independent of SessionStore so tests can exercise the policy
// logic against fakes.
#pragma once

#include <chrono>
#include <cstddef>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <hermes/gateway/gateway_config.hpp>

namespace hermes::gateway {

// --- TTL tracker ---------------------------------------------------------

struct SessionTtlEntry {
    std::chrono::system_clock::time_point created_at;
    std::chrono::system_clock::time_point last_activity;
    std::chrono::system_clock::time_point expires_at;
    std::size_t turn_count = 0;
    std::size_t message_count = 0;
    std::size_t size_bytes = 0;
    bool pinned = false;
};

struct CompactionTriggers {
    std::size_t max_turns = 200;
    std::size_t max_messages = 1000;
    std::size_t max_bytes = 1024 * 1024;    // 1 MiB
    std::chrono::seconds max_age{7 * 24 * 3600};
    std::chrono::seconds max_idle{24 * 3600};
};

enum class CompactionReason {
    None,
    TurnCount,
    MessageCount,
    SizeBytes,
    Age,
    Idle,
    Manual,
};

const char* compaction_reason_name(CompactionReason reason);

class SessionTtlTracker {
public:
    SessionTtlTracker();

    void set_triggers(CompactionTriggers t);
    CompactionTriggers triggers() const;

    // Default TTL applied at creation time.  Sessions can have their
    // expires_at explicitly overridden via ``extend``.
    void set_default_ttl(std::chrono::seconds ttl);
    std::chrono::seconds default_ttl() const;

    // Register or update a session.
    void touch(const std::string& session_key,
                std::chrono::system_clock::time_point now);

    // Record a new turn / message.
    void record_turn(const std::string& session_key,
                      std::size_t message_bytes);

    // Mark a session as pinned (skips compaction/eviction).
    void pin(const std::string& session_key, bool pinned);

    // Extend the expiry on a session.  Absolute, not additive.
    void extend(const std::string& session_key,
                 std::chrono::system_clock::time_point until);

    // Return the recorded entry (nullopt if unknown).
    std::optional<SessionTtlEntry> entry(
        const std::string& session_key) const;

    // Every session, sorted by key.
    std::vector<std::pair<std::string, SessionTtlEntry>> entries() const;

    // Which sessions are past their ``expires_at`` given ``now``.
    std::vector<std::string> expired(
        std::chrono::system_clock::time_point now) const;

    // For each session, determine whether any compaction trigger has
    // fired.  Returns pairs of (session_key, reason).
    std::vector<std::pair<std::string, CompactionReason>> due_for_compaction(
        std::chrono::system_clock::time_point now) const;

    // Erase every bookkeeping record for ``session_key``.
    void forget(const std::string& session_key);

    // Session count.
    std::size_t size() const;

    // Total bytes across all tracked sessions.
    std::size_t total_bytes() const;

private:
    mutable std::mutex mu_;
    std::unordered_map<std::string, SessionTtlEntry> entries_;
    CompactionTriggers triggers_;
    std::chrono::seconds default_ttl_{7 * 24 * 3600};
};

// --- Insight recorder ----------------------------------------------------

struct SessionInsight {
    std::string session_key;
    std::string category;       // e.g. "summary", "memory", "score"
    std::string content;
    double score = 0.0;
    std::vector<std::string> tags;
    std::chrono::system_clock::time_point recorded_at;
};

class InsightLedger {
public:
    InsightLedger();

    void record(SessionInsight insight);

    // Insights for a given session (in recording order).
    std::vector<SessionInsight> for_session(
        const std::string& session_key) const;

    // All insights matching a category, in recording order.
    std::vector<SessionInsight> by_category(
        const std::string& category) const;

    // Top-N insights by score for a session (descending score).
    std::vector<SessionInsight> top_for_session(
        const std::string& session_key, std::size_t n) const;

    // Drop every insight for a given session.
    std::size_t purge_session(const std::string& session_key);

    // Total recorded count.
    std::size_t size() const;

    // Distinct categories, sorted.
    std::vector<std::string> categories() const;

    void clear();

private:
    mutable std::mutex mu_;
    std::vector<SessionInsight> ledger_;
};

// --- Cross-platform linker -----------------------------------------------

// Links multiple platform-specific identifiers to a single canonical
// user / tenant id.  Used by gateway/run.py to unify "same person on
// Telegram and Slack" so memory / preferences carry across.
class SessionLinker {
public:
    SessionLinker();

    // Associate a platform user id with a canonical id.  If
    // ``canonical_id`` is empty a new one is generated.
    std::string link(Platform platform, const std::string& platform_user_id,
                      std::string canonical_id = {});

    // Unlink a specific platform identity.  Returns true if anything
    // was removed.
    bool unlink(Platform platform, const std::string& platform_user_id);

    // Canonical id for a platform user (nullopt if not linked).
    std::optional<std::string> canonical_for(
        Platform platform, const std::string& platform_user_id) const;

    // All platform identities mapped to a canonical id.
    std::vector<std::pair<Platform, std::string>> identities_for(
        const std::string& canonical_id) const;

    // Number of canonical ids known.
    std::size_t canonical_count() const;

    // Total identity rows.
    std::size_t identity_count() const;

    void clear();

private:
    mutable std::mutex mu_;
    std::unordered_map<std::string, std::string> by_composite_;  // "platform:id" -> canonical
    std::unordered_map<std::string,
                        std::vector<std::pair<Platform, std::string>>>
        by_canonical_;
    std::size_t counter_ = 0;
};

}  // namespace hermes::gateway
