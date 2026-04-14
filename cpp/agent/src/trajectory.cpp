// C++17 port of agent/trajectory.py + agent/manual_compression_feedback.py.
#include "hermes/agent/trajectory.hpp"

#include <chrono>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <string>
#include <system_error>

namespace hermes::agent::trajectory {

namespace {

std::string iso8601_now() {
    using namespace std::chrono;
    const auto now = system_clock::now();
    const auto t = system_clock::to_time_t(now);
    const auto ms =
        duration_cast<microseconds>(now.time_since_epoch()).count() % 1000000;
    std::tm tm{};
#if defined(_WIN32)
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    std::ostringstream os;
    os << std::put_time(&tm, "%Y-%m-%dT%H:%M:%S");
    os << '.' << std::setw(6) << std::setfill('0') << ms;
    return os.str();
}

// Replace every occurrence of `needle` in `text` with `repl`.
std::string replace_all(std::string text, const std::string& needle,
                        const std::string& repl) {
    if (needle.empty()) return text;
    std::string out;
    out.reserve(text.size());
    std::size_t pos = 0;
    std::size_t prev = 0;
    while ((pos = text.find(needle, prev)) != std::string::npos) {
        out.append(text, prev, pos - prev);
        out.append(repl);
        prev = pos + needle.size();
    }
    out.append(text, prev, text.size() - prev);
    return out;
}

}  // namespace

std::string convert_scratchpad_to_think(const std::string& content) {
    static const std::string open = "<REASONING_SCRATCHPAD>";
    if (content.empty() || content.find(open) == std::string::npos) {
        return content;
    }
    std::string s = replace_all(content, open, "<think>");
    s = replace_all(s, "</REASONING_SCRATCHPAD>", "</think>");
    return s;
}

bool has_incomplete_scratchpad(const std::string& content) {
    if (content.empty()) return false;
    const bool has_open = content.find("<REASONING_SCRATCHPAD>") != std::string::npos;
    const bool has_close = content.find("</REASONING_SCRATCHPAD>") != std::string::npos;
    return has_open && !has_close;
}

bool save_trajectory(const nlohmann::json& conversations,
                     const std::string& model,
                     bool completed,
                     const std::string& filename) {
    std::string path = filename;
    if (path.empty()) {
        path = completed ? "trajectory_samples.jsonl" : "failed_trajectories.jsonl";
    }

    nlohmann::json entry = {
        {"conversations", conversations},
        {"timestamp", iso8601_now()},
        {"model", model},
        {"completed", completed},
    };

    try {
        std::ofstream os(path, std::ios::app);
        if (!os) return false;
        os << entry.dump() << '\n';
        return static_cast<bool>(os);
    } catch (...) {
        return false;
    }
}

}  // namespace hermes::agent::trajectory

// ── manual compression feedback ────────────────────────────────────────

namespace hermes::agent::compression_feedback {

namespace {

std::string fmt_with_commas(long long n) {
    std::string raw = std::to_string(n);
    std::string out;
    int cnt = 0;
    for (auto it = raw.rbegin(); it != raw.rend(); ++it) {
        if (*it == '-') {
            out.push_back(*it);
            continue;
        }
        if (cnt > 0 && cnt % 3 == 0) out.push_back(',');
        out.push_back(*it);
        ++cnt;
    }
    std::reverse(out.begin(), out.end());
    return out;
}

}  // namespace

ManualCompressionSummary summarize_manual_compression(
    const std::vector<nlohmann::json>& before_messages,
    const std::vector<nlohmann::json>& after_messages,
    long long before_tokens,
    long long after_tokens) {
    ManualCompressionSummary s;
    const auto before_count = before_messages.size();
    const auto after_count = after_messages.size();
    s.noop = before_messages == after_messages;

    if (s.noop) {
        s.headline = "No changes from compression: " +
                     std::to_string(before_count) + " messages";
        if (after_tokens == before_tokens) {
            s.token_line = "Rough transcript estimate: ~" +
                           fmt_with_commas(before_tokens) +
                           " tokens (unchanged)";
        } else {
            s.token_line = "Rough transcript estimate: ~" +
                           fmt_with_commas(before_tokens) + " → ~" +
                           fmt_with_commas(after_tokens) + " tokens";
        }
    } else {
        s.headline = "Compressed: " + std::to_string(before_count) + " → " +
                     std::to_string(after_count) + " messages";
        s.token_line = "Rough transcript estimate: ~" +
                       fmt_with_commas(before_tokens) + " → ~" +
                       fmt_with_commas(after_tokens) + " tokens";
    }

    if (!s.noop && after_count < before_count && after_tokens > before_tokens) {
        s.note =
            "Note: fewer messages can still raise this rough transcript "
            "estimate when compression rewrites the transcript into denser "
            "summaries.";
    }
    return s;
}

}  // namespace hermes::agent::compression_feedback
