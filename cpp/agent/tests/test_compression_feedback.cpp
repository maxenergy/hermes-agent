#include "hermes/agent/compression_feedback.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>

using hermes::agent::CompressionEvent;
using hermes::agent::CompressionFeedbackCollector;
using hermes::agent::CompressionVerdict;

namespace {

std::filesystem::path tmp_feedback_path() {
    auto dir = std::filesystem::temp_directory_path() /
               ("hermes_cmpfb_" +
                std::to_string(
                    std::chrono::system_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(dir);
    return dir / "compression_feedback.jsonl";
}

}  // namespace

TEST(CompressionFeedback, ParseVerdictAccepts) {
    using V = CompressionVerdict;
    EXPECT_EQ(CompressionFeedbackCollector::parse_verdict("good").value(),
              V::Good);
    EXPECT_EQ(CompressionFeedbackCollector::parse_verdict("bad").value(),
              V::Bad);
    EXPECT_EQ(CompressionFeedbackCollector::parse_verdict("UP").value(), V::Good);
    EXPECT_EQ(CompressionFeedbackCollector::parse_verdict("-1").value(), V::Bad);
    EXPECT_FALSE(
        CompressionFeedbackCollector::parse_verdict("maybe").has_value());
}

TEST(CompressionFeedback, RegisterThenRecord) {
    auto path = tmp_feedback_path();
    CompressionFeedbackCollector c(path);
    CompressionEvent ev;
    ev.session_id = "sess-1";
    ev.messages_before = 20;
    ev.messages_after = 7;
    ev.tokens_before = 12000;
    ev.tokens_after = 2500;
    ev.summary_excerpt = "goal: ship feature";
    auto id = c.register_event(ev);
    EXPECT_FALSE(id.empty());
    EXPECT_EQ(c.pending_events().size(), 1u);

    ASSERT_TRUE(c.record_feedback(CompressionVerdict::Good, "looks right"));
    EXPECT_EQ(c.pending_events().size(), 0u);

    auto records = c.load_all();
    ASSERT_EQ(records.size(), 1u);
    EXPECT_EQ(records[0].event.event_id, id);
    EXPECT_EQ(records[0].event.session_id, "sess-1");
    EXPECT_EQ(records[0].event.messages_before, 20);
    EXPECT_EQ(records[0].note, "looks right");
    EXPECT_EQ(records[0].verdict, CompressionVerdict::Good);
    std::filesystem::remove_all(path.parent_path());
}

TEST(CompressionFeedback, RecordBadByExplicitId) {
    auto path = tmp_feedback_path();
    CompressionFeedbackCollector c(path);
    auto a = c.register_event({});
    auto b = c.register_event({});
    ASSERT_TRUE(
        c.record_feedback(CompressionVerdict::Bad, "skipped too much", a));
    auto pending = c.pending_events();
    ASSERT_EQ(pending.size(), 1u);
    EXPECT_EQ(pending[0].event_id, b);

    auto records = c.load_all();
    ASSERT_EQ(records.size(), 1u);
    EXPECT_EQ(records[0].event.event_id, a);
    EXPECT_EQ(records[0].verdict, CompressionVerdict::Bad);
    std::filesystem::remove_all(path.parent_path());
}

TEST(CompressionFeedback, RecordWithoutEventReturnsFalse) {
    auto path = tmp_feedback_path();
    CompressionFeedbackCollector c(path);
    EXPECT_FALSE(c.record_feedback(CompressionVerdict::Good));
    EXPECT_FALSE(std::filesystem::exists(path));
    std::filesystem::remove_all(path.parent_path());
}

TEST(CompressionFeedback, RecordUnknownIdReturnsFalse) {
    auto path = tmp_feedback_path();
    CompressionFeedbackCollector c(path);
    c.register_event({});
    EXPECT_FALSE(
        c.record_feedback(CompressionVerdict::Good, "note", "cmp_missing"));
    EXPECT_EQ(c.pending_events().size(), 1u);
    std::filesystem::remove_all(path.parent_path());
}

TEST(CompressionFeedback, LoadSkipsBadLines) {
    auto path = tmp_feedback_path();
    {
        std::ofstream out(path);
        out << "not json\n";
        out << R"({"event_id":"cmp_ok","verdict":"bad","note":"ok"})" << '\n';
        out << "\n";  // blank
    }
    CompressionFeedbackCollector c(path);
    auto records = c.load_all();
    ASSERT_EQ(records.size(), 1u);
    EXPECT_EQ(records[0].event.event_id, "cmp_ok");
    EXPECT_EQ(records[0].verdict, CompressionVerdict::Bad);
    std::filesystem::remove_all(path.parent_path());
}

TEST(CompressionFeedback, LastEventFollowsFifo) {
    auto path = tmp_feedback_path();
    CompressionFeedbackCollector c(path);
    CompressionEvent a;
    a.session_id = "A";
    CompressionEvent b;
    b.session_id = "B";
    c.register_event(a);
    c.register_event(b);
    auto last = c.last_event();
    ASSERT_TRUE(last.has_value());
    EXPECT_EQ(last->session_id, "B");
    std::filesystem::remove_all(path.parent_path());
}
