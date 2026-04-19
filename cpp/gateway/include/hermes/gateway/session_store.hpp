// File-based session persistence for gateway.
#pragma once

#include <chrono>
#include <filesystem>
#include <map>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <nlohmann/json.hpp>

#include <hermes/gateway/gateway_config.hpp>

namespace hermes::gateway {

struct SessionSource {
    Platform platform;
    std::string chat_id, chat_name, chat_type;  // dm|group|channel|thread
    std::string user_id, user_name;
    std::string thread_id, chat_topic;
    std::string user_id_alt, chat_id_alt;  // Signal UUID etc.

    std::string description() const;
    nlohmann::json to_json() const;
    static SessionSource from_json(const nlohmann::json& j);
};

struct SessionContext {
    SessionSource source;
    std::vector<Platform> connected_platforms;
    std::map<Platform, HomeChannel> home_channels;
    std::string session_key, session_id;
    std::chrono::system_clock::time_point created_at, updated_at;

    // Hard forced-wipe signal (/stop, stuck-loop escalation).  Next access
    // returns a fresh ``session_id``.  Always wins over ``resume_pending``.
    bool suspended = false;

    // Restart-interrupted session: preserve ``session_id`` + transcript on
    // next access so the user resumes the same turn.  Set by drain-timeout
    // shutdown (gateway/run.py _stop_impl — upstream cb4addac, c49a58a6).
    // Cleared after the next successful turn.  Terminal escalation flows
    // through the existing ``restart_failure_counts`` stuck-loop counter
    // (threshold 3, PR #7536) — no parallel counter on the entry.
    bool resume_pending = false;
    std::string resume_reason;  // e.g. "restart_timeout"
    std::chrono::system_clock::time_point last_resume_marked_at{};

    nlohmann::json to_json() const;
};

class SessionStore {
public:
    // Matches Python ``.restart_failure_counts`` threshold (PR #7536):
    // once a session has failed to complete a resumed turn this many times
    // in a row, the next access wipes the transcript and starts fresh.
    static constexpr int kResumeFailureThreshold = 3;

    explicit SessionStore(std::filesystem::path sessions_dir);

    // ``respect_resume_pending=true`` (the default) honors the
    // ``resume_pending`` flag: returns the existing session_id without
    // touching the transcript.  ``false`` ignores the flag (used by /reset).
    std::string get_or_create_session(const SessionSource& source);
    void reset_session(const std::string& session_key);
    void append_message(const std::string& session_key,
                        const nlohmann::json& message);
    std::vector<nlohmann::json> load_transcript(
        const std::string& session_key);

    bool should_reset(const std::string& session_key,
                      const SessionResetPolicy& policy);

    // --- Resume-pending helpers (upstream cb4addac / c49a58a6) ----------

    // Flag an existing session so the next inbound message on the same
    // session_key preserves session_id + transcript.  Returns true if the
    // session existed and was marked.  ``suspended`` always wins and is
    // not overridden.
    bool mark_resume_pending(const std::string& session_key,
                              const std::string& reason = "restart_timeout");

    // Clear the flag after a successful resumed turn.  Also resets the
    // restart-failure counter.  Returns true if a flag was cleared.
    bool clear_resume_pending(const std::string& session_key);

    // Query current state.  Exposed for the runner + tests.
    bool is_resume_pending(const std::string& session_key) const;
    std::string resume_reason(const std::string& session_key) const;
    bool is_suspended(const std::string& session_key) const;
    int restart_failure_count(const std::string& session_key) const;

    // Returns true when the previous resumed turn succeeded and this call
    // consumed the flag.  Used by the runner to decide whether to inject
    // the restart-resume system note.
    struct ResumeDecision {
        bool should_resume = false;
        std::string reason;            // ``resume_reason`` if any
        int failure_count = 0;         // current restart-failure counter
        bool escalated_to_suspended = false;  // threshold crossed -> wiped
    };
    ResumeDecision consume_resume_pending(const std::string& session_key);

    // Bump the restart-failure counter when a resumed turn fails to
    // complete cleanly.  Once it reaches ``kResumeFailureThreshold`` the
    // session is promoted to ``suspended`` on the next access.
    int bump_restart_failure_count(const std::string& session_key);
    void clear_restart_failure_count(const std::string& session_key);

    // --- Stale pruning (upstream eb07c056) ------------------------------

    // Drop SessionContext records whose ``updated_at`` is older than
    // ``max_age_days`` days.  Matches Python SessionStore.prune_old_entries:
    //   - ``max_age_days <= 0`` returns 0 immediately
    //   - ``suspended`` entries are kept (user paused them for later resume)
    //   - transcript / sidecar counters are NOT removed — pruning is
    //     functionally identical to a natural reset-policy expiry
    //
    // Returns the number of entries removed.
    std::size_t prune_old_entries(int max_age_days);

    // Flush in-flight changes + close any OS-level handles so a
    // --replace restart can open the same session directory without
    // contention.  Safe to call multiple times (idempotent).  After
    // close() other accessors still function — they will re-open the
    // per-session files on demand.
    void close();

    // PII redaction: hash IDs for Telegram/Signal/WhatsApp/BlueBubbles
    // Discord excluded (needs real IDs for <@user_id> mentions)
    static std::string hash_id(std::string_view id);  // SHA256 12-char hex
    SessionSource redact_pii(const SessionSource& source) const;

private:
    std::filesystem::path dir_;
    mutable std::recursive_mutex mu_;
    // Each session: {dir}/{session_key}/session.json + transcript.jsonl
    // Restart-failure counter sidecar: {dir}/.restart_failure_counts.json.

    // Internal helpers — caller must hold mu_.
    std::filesystem::path counters_path_() const;
    std::unordered_map<std::string, int> load_counters_locked_() const;
    void save_counters_locked_(
        const std::unordered_map<std::string, int>& counters) const;
};

}  // namespace hermes::gateway
