#include "hermes/agent/compression_feedback.hpp"

#include "hermes/core/path.hpp"
#include "hermes/core/time.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <random>
#include <sstream>

namespace hermes::agent {

namespace {

std::string verdict_to_str(CompressionVerdict v) {
    return v == CompressionVerdict::Good ? "good" : "bad";
}

std::optional<CompressionVerdict> str_to_verdict(const std::string& s) {
    std::string lc(s);
    std::transform(lc.begin(), lc.end(), lc.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    if (lc == "good" || lc == "up" || lc == "+1" || lc == "+" || lc == "y" ||
        lc == "yes") {
        return CompressionVerdict::Good;
    }
    if (lc == "bad" || lc == "down" || lc == "-1" || lc == "-" || lc == "n" ||
        lc == "no") {
        return CompressionVerdict::Bad;
    }
    return std::nullopt;
}

}  // namespace

std::string CompressionFeedbackCollector::make_event_id() {
    static constexpr char kAlpha[] =
        "0123456789abcdefghijklmnopqrstuvwxyz";
    static thread_local std::mt19937_64 rng{std::random_device{}()};
    std::uniform_int_distribution<std::size_t> dist(0, sizeof(kAlpha) - 2);
    std::string id = "cmp_";
    for (int i = 0; i < 10; ++i) id.push_back(kAlpha[dist(rng)]);
    return id;
}

std::optional<CompressionVerdict>
CompressionFeedbackCollector::parse_verdict(const std::string& token) {
    return str_to_verdict(token);
}

CompressionFeedbackCollector::CompressionFeedbackCollector()
    : CompressionFeedbackCollector(
          hermes::core::path::get_hermes_home() /
          "compression_feedback.jsonl") {}

CompressionFeedbackCollector::CompressionFeedbackCollector(
    std::filesystem::path path)
    : path_(std::move(path)) {
    std::error_code ec;
    std::filesystem::create_directories(path_.parent_path(), ec);
}

std::string CompressionFeedbackCollector::register_event(
    CompressionEvent event) {
    std::lock_guard<std::mutex> lock(mu_);
    if (event.event_id.empty()) event.event_id = make_event_id();
    auto id = event.event_id;
    pending_.push_back(std::move(event));
    // Cap pending list so abandoned events don't grow unbounded.
    if (pending_.size() > 64) {
        pending_.erase(pending_.begin(),
                       pending_.begin() +
                           static_cast<std::ptrdiff_t>(pending_.size() - 64));
    }
    return id;
}

bool CompressionFeedbackCollector::record_feedback(
    CompressionVerdict verdict, const std::string& note,
    const std::string& event_id) {
    std::unique_lock<std::mutex> lock(mu_);
    if (pending_.empty()) return false;
    auto it = pending_.end();
    if (event_id.empty()) {
        it = std::prev(pending_.end());
    } else {
        it = std::find_if(pending_.begin(), pending_.end(),
                          [&](const auto& e) { return e.event_id == event_id; });
    }
    if (it == pending_.end()) return false;
    CompressionEvent ev = std::move(*it);
    pending_.erase(it);

    nlohmann::json j;
    j["event_id"]        = ev.event_id;
    j["session_id"]      = ev.session_id;
    j["verdict"]         = verdict_to_str(verdict);
    j["note"]            = note;
    j["messages_before"] = ev.messages_before;
    j["messages_after"]  = ev.messages_after;
    j["tokens_before"]   = ev.tokens_before;
    j["tokens_after"]    = ev.tokens_after;
    j["summary_excerpt"] = ev.summary_excerpt;
    j["recorded_at"]     = hermes::core::time::format_iso8601(
        hermes::core::time::now());

    auto line = j.dump();
    lock.unlock();
    append_line_(line);
    return true;
}

void CompressionFeedbackCollector::append_line_(const std::string& line) const {
    std::ofstream out(path_, std::ios::app | std::ios::binary);
    if (!out) return;  // best-effort
    out << line << '\n';
}

std::vector<CompressionEvent>
CompressionFeedbackCollector::pending_events() const {
    std::lock_guard<std::mutex> lock(mu_);
    return pending_;
}

std::optional<CompressionEvent>
CompressionFeedbackCollector::last_event() const {
    std::lock_guard<std::mutex> lock(mu_);
    if (pending_.empty()) return std::nullopt;
    return pending_.back();
}

std::vector<CompressionFeedbackRecord>
CompressionFeedbackCollector::load_all() const {
    std::lock_guard<std::mutex> lock(mu_);
    std::vector<CompressionFeedbackRecord> out;
    std::ifstream in(path_);
    if (!in) return out;
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty()) continue;
        try {
            auto j = nlohmann::json::parse(line);
            CompressionFeedbackRecord rec;
            rec.event.event_id        = j.value("event_id", "");
            rec.event.session_id      = j.value("session_id", "");
            rec.event.messages_before = j.value("messages_before", 0);
            rec.event.messages_after  = j.value("messages_after", 0);
            rec.event.tokens_before   = j.value("tokens_before", std::int64_t{0});
            rec.event.tokens_after    = j.value("tokens_after", std::int64_t{0});
            rec.event.summary_excerpt = j.value("summary_excerpt", "");
            rec.note                  = j.value("note", "");
            rec.recorded_at           = j.value("recorded_at", "");
            auto vstr = j.value("verdict", "good");
            rec.verdict = (vstr == "bad") ? CompressionVerdict::Bad
                                          : CompressionVerdict::Good;
            out.push_back(std::move(rec));
        } catch (...) {
            // skip malformed line
        }
    }
    return out;
}

}  // namespace hermes::agent
