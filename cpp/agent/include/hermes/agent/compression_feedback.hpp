// compression_feedback — capture user verdicts on compression events.
//
// After ContextCompressor replaces the middle of the conversation with
// a summary system message, the UI layer can prompt the user ("was
// that summary accurate?") and record the verdict here.  Every
// verdict is appended to `{HERMES_HOME}/compression_feedback.jsonl`
// for later offline analysis or prompt tuning.
//
// Each line of the JSONL file has the schema:
//   {
//     "event_id":   "<uuid>",
//     "session_id": "<session>",
//     "verdict":    "good"|"bad",
//     "note":       "<free text>",
//     "messages_before": 12,
//     "messages_after":  6,
//     "tokens_before":   12345,
//     "tokens_after":     2345,
//     "recorded_at":     "2025-01-01T12:34:56+08:00"
//   }
#pragma once

#include <cstdint>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace hermes::agent {

enum class CompressionVerdict {
    Good,
    Bad,
};

struct CompressionEvent {
    // Opaque id the UI displays to the user (`/compress feedback
    // <verdict> [note]` refers to the most recent event by default).
    std::string event_id;
    std::string session_id;
    int messages_before = 0;
    int messages_after = 0;
    std::int64_t tokens_before = 0;
    std::int64_t tokens_after = 0;
    std::string summary_excerpt;  // first ~200 chars, for display
};

struct CompressionFeedbackRecord {
    CompressionEvent event;
    CompressionVerdict verdict = CompressionVerdict::Good;
    std::string note;
    std::string recorded_at;  // ISO-8601
};

class CompressionFeedbackCollector {
public:
    // Default ctor uses `{HERMES_HOME}/compression_feedback.jsonl`.
    CompressionFeedbackCollector();

    // Overrideable path for tests.
    explicit CompressionFeedbackCollector(std::filesystem::path path);

    // Register a new compression event.  The returned event_id should
    // be handed to the UI so the user can reference it later.
    std::string register_event(CompressionEvent event);

    // Record a verdict for an event id.  If event_id is empty, applies
    // to the most-recently-registered event.  Returns false if no
    // matching event exists.
    bool record_feedback(CompressionVerdict verdict,
                         const std::string& note = {},
                         const std::string& event_id = {});

    // Parse a verdict word ("good"/"bad"/"up"/"down"/"+"/"-").
    static std::optional<CompressionVerdict> parse_verdict(
        const std::string& token);

    // Accessors (for tests + `/compress status` UX).
    std::vector<CompressionEvent> pending_events() const;
    std::optional<CompressionEvent> last_event() const;

    // Load all historical records.  File is parsed line-by-line; bad
    // lines are skipped silently.
    std::vector<CompressionFeedbackRecord> load_all() const;

    const std::filesystem::path& path() const { return path_; }

    // Generate a short random event id (used internally, exposed for
    // tests that need deterministic setup via register_event).
    static std::string make_event_id();

private:
    mutable std::mutex mu_;
    std::filesystem::path path_;
    std::vector<CompressionEvent> pending_;  // FIFO of open events

    void append_line_(const std::string& line) const;
};

}  // namespace hermes::agent
