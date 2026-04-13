#include "hermes/agent/insights.hpp"

#include "hermes/core/path.hpp"
#include "hermes/core/time.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <unordered_set>

namespace hermes::agent {

namespace {

const char* kind_str(InsightEvent::Kind k) {
    return k == InsightEvent::Kind::ModelTurn ? "model_turn" : "tool_call";
}

InsightEvent::Kind parse_kind(const std::string& s) {
    return s == "tool_call" ? InsightEvent::Kind::ToolCall
                            : InsightEvent::Kind::ModelTurn;
}

std::string format_iso(std::chrono::system_clock::time_point tp) {
    return hermes::core::time::format_iso8601(tp);
}

std::chrono::system_clock::time_point parse_iso(const std::string& s) {
    // Minimal parser: YYYY-MM-DDTHH:MM:SS (+|-)HH:MM.  Timezone offset
    // is applied; fractional seconds are ignored.
    if (s.size() < 19) return {};
    std::tm tm{};
    int off_h = 0, off_m = 0;
    char sign = '+';
    if (std::sscanf(s.c_str(),
                    "%4d-%2d-%2dT%2d:%2d:%2d%c%2d:%2d",
                    &tm.tm_year, &tm.tm_mon, &tm.tm_mday,
                    &tm.tm_hour, &tm.tm_min, &tm.tm_sec,
                    &sign, &off_h, &off_m) < 6) {
        return {};
    }
    tm.tm_year -= 1900;
    tm.tm_mon -= 1;
#if defined(_WIN32)
    auto t = _mkgmtime(&tm);
#else
    auto t = timegm(&tm);
#endif
    int off = off_h * 60 + off_m;
    if (sign == '-') off = -off;
    t -= off * 60;
    return std::chrono::system_clock::from_time_t(t);
}

}  // namespace

InsightsRecorder::InsightsRecorder()
    : InsightsRecorder(hermes::core::path::get_hermes_home() / "insights.jsonl") {}

InsightsRecorder::InsightsRecorder(std::filesystem::path path)
    : path_(std::move(path)) {
    std::error_code ec;
    std::filesystem::create_directories(path_.parent_path(), ec);
}

void InsightsRecorder::record(const InsightEvent& ev) {
    nlohmann::json j;
    j["kind"]          = kind_str(ev.kind);
    j["session_id"]    = ev.session_id;
    j["model"]         = ev.model;
    j["tool_name"]     = ev.tool_name;
    j["input_tokens"]  = ev.input_tokens;
    j["output_tokens"] = ev.output_tokens;
    j["cost_usd"]      = ev.cost_usd;
    j["latency_ms"]    = ev.latency_ms;
    j["error"]         = ev.error;
    j["at"] = format_iso(ev.at.time_since_epoch().count() == 0
                             ? hermes::core::time::now()
                             : ev.at);
    auto line = j.dump();
    std::lock_guard<std::mutex> lock(mu_);
    append_locked_(line);
}

void InsightsRecorder::record_model_turn(const std::string& session_id,
                                         const std::string& model,
                                         std::int64_t input_tokens,
                                         std::int64_t output_tokens,
                                         double cost_usd, double latency_ms) {
    InsightEvent ev;
    ev.kind = InsightEvent::Kind::ModelTurn;
    ev.session_id = session_id;
    ev.model = model;
    ev.input_tokens = input_tokens;
    ev.output_tokens = output_tokens;
    ev.cost_usd = cost_usd;
    ev.latency_ms = latency_ms;
    ev.at = hermes::core::time::now();
    record(ev);
}

void InsightsRecorder::record_tool_call(const std::string& session_id,
                                        const std::string& tool_name,
                                        double latency_ms, bool error) {
    InsightEvent ev;
    ev.kind = InsightEvent::Kind::ToolCall;
    ev.session_id = session_id;
    ev.tool_name = tool_name;
    ev.latency_ms = latency_ms;
    ev.error = error;
    ev.at = hermes::core::time::now();
    record(ev);
}

void InsightsRecorder::append_locked_(const std::string& line) const {
    std::ofstream out(path_, std::ios::app | std::ios::binary);
    if (!out) return;
    out << line << '\n';
}

std::vector<InsightEvent> InsightsRecorder::load_all() const {
    std::lock_guard<std::mutex> lock(mu_);
    std::vector<InsightEvent> out;
    std::ifstream in(path_);
    if (!in) return out;
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty()) continue;
        try {
            auto j = nlohmann::json::parse(line);
            InsightEvent ev;
            ev.kind = parse_kind(j.value("kind", "model_turn"));
            ev.session_id    = j.value("session_id", "");
            ev.model         = j.value("model", "");
            ev.tool_name     = j.value("tool_name", "");
            ev.input_tokens  = j.value("input_tokens", std::int64_t{0});
            ev.output_tokens = j.value("output_tokens", std::int64_t{0});
            ev.cost_usd      = j.value("cost_usd", 0.0);
            ev.latency_ms    = j.value("latency_ms", 0.0);
            ev.error         = j.value("error", false);
            ev.at            = parse_iso(j.value("at", std::string{}));
            out.push_back(std::move(ev));
        } catch (...) {
            // skip malformed
        }
    }
    return out;
}

namespace {

double percentile(std::vector<double> v, double pct) {
    if (v.empty()) return 0.0;
    std::sort(v.begin(), v.end());
    auto n = v.size();
    auto idx = static_cast<std::size_t>(std::floor(pct * (n - 1)));
    if (idx >= n) idx = n - 1;
    return v[idx];
}

}  // namespace

InsightSummary InsightsRecorder::summarize(
    const std::vector<InsightEvent>& events,
    std::chrono::system_clock::time_point cutoff) {
    InsightSummary sum;
    std::unordered_set<std::string> sessions;
    std::vector<double> turn_latencies;
    for (const auto& ev : events) {
        if (ev.at < cutoff) continue;
        if (!ev.session_id.empty()) sessions.insert(ev.session_id);
        if (ev.kind == InsightEvent::Kind::ModelTurn) {
            ++sum.model_turns;
            sum.input_tokens += ev.input_tokens;
            sum.output_tokens += ev.output_tokens;
            sum.cost_usd += ev.cost_usd;
            if (ev.latency_ms > 0) turn_latencies.push_back(ev.latency_ms);
            if (!ev.model.empty()) ++sum.model_turn_counts[ev.model];
        } else {
            ++sum.tool_calls;
            if (!ev.tool_name.empty()) ++sum.tool_call_counts[ev.tool_name];
        }
    }
    sum.sessions = static_cast<int>(sessions.size());
    sum.latency_p50_ms = percentile(turn_latencies, 0.50);
    sum.latency_p95_ms = percentile(turn_latencies, 0.95);
    return sum;
}

InsightSummary InsightsRecorder::summarize_last_days(int days) const {
    auto cutoff = hermes::core::time::now() -
                  std::chrono::hours(24 * std::max(1, days));
    return summarize(load_all(), cutoff);
}

std::string InsightSummary::render() const {
    std::ostringstream os;
    os << "Sessions:       " << sessions << "\n";
    os << "Model turns:    " << model_turns << "\n";
    os << "Tool calls:     " << tool_calls << "\n";
    os << "Input tokens:   " << input_tokens << "\n";
    os << "Output tokens:  " << output_tokens << "\n";
    os << "Est. cost:      $" << std::fixed << std::setprecision(4)
       << cost_usd << "\n";
    os << "Latency p50:    " << std::fixed << std::setprecision(1)
       << latency_p50_ms << " ms\n";
    os << "Latency p95:    " << std::fixed << std::setprecision(1)
       << latency_p95_ms << " ms\n";
    if (!tool_call_counts.empty()) {
        os << "Top tools:\n";
        std::vector<std::pair<std::string, int>> tools(
            tool_call_counts.begin(), tool_call_counts.end());
        std::sort(tools.begin(), tools.end(),
                  [](const auto& a, const auto& b) {
                      return a.second > b.second;
                  });
        std::size_t n = std::min<std::size_t>(tools.size(), 5);
        for (std::size_t i = 0; i < n; ++i) {
            os << "  " << tools[i].first << "  × " << tools[i].second << "\n";
        }
    }
    return os.str();
}

}  // namespace hermes::agent
