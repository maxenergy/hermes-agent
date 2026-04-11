#include "hermes/state/trajectory.hpp"

#include "hermes/core/path.hpp"

#include <chrono>
#include <cstdio>
#include <ctime>
#include <fcntl.h>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <string>
#include <system_error>
#include <unistd.h>

namespace hermes::state {

namespace {

const std::string kScratchOpen = "<REASONING_SCRATCHPAD>";
const std::string kScratchClose = "</REASONING_SCRATCHPAD>";
const std::string kThinkOpen = "<think>";
const std::string kThinkClose = "</think>";

std::string iso8601_now_if_empty(const std::string& s) {
    if (!s.empty()) return s;
    using namespace std::chrono;
    auto now = system_clock::now();
    auto t = system_clock::to_time_t(now);
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    auto ms = duration_cast<milliseconds>(now.time_since_epoch()).count() % 1000;
    std::ostringstream oss;
    oss << std::put_time(&tm, "%Y-%m-%dT%H:%M:%S") << '.'
        << std::setw(3) << std::setfill('0') << ms << 'Z';
    return oss.str();
}

}  // namespace

TrajectoryWriter::TrajectoryWriter()
    : TrajectoryWriter(hermes::core::path::get_hermes_home() /
                       "trajectories" / "trajectory_samples.jsonl") {}

TrajectoryWriter::TrajectoryWriter(const std::filesystem::path& jsonl_path)
    : path_(jsonl_path) {
    std::error_code ec;
    std::filesystem::create_directories(path_.parent_path(), ec);
    (void)ec;
}

void TrajectoryWriter::write(const TrajectoryRecord& rec) {
    nlohmann::json j;
    j["conversations"] = rec.messages;
    j["timestamp"] = iso8601_now_if_empty(rec.timestamp);
    j["model"] = rec.model;
    j["completed"] = rec.completed;
    if (rec.error.has_value()) {
        j["error"] = *rec.error;
    }

    // O_APPEND writes up to PIPE_BUF are atomic on POSIX — suitable for
    // JSONL where each record is a single line. For larger records we
    // still rely on the OS to sequence the fwrite.
    std::ofstream out(path_, std::ios::out | std::ios::app | std::ios::binary);
    if (!out) return;
    out << j.dump() << '\n';
}

std::string TrajectoryWriter::convert_scratchpad_to_think(
    std::string_view text) {
    std::string s(text);
    if (s.find(kScratchOpen) == std::string::npos) return s;

    std::string out;
    out.reserve(s.size());
    std::size_t pos = 0;
    while (pos < s.size()) {
        auto open_at = s.find(kScratchOpen, pos);
        if (open_at == std::string::npos) {
            out.append(s, pos, std::string::npos);
            break;
        }
        out.append(s, pos, open_at - pos);
        out.append(kThinkOpen);
        pos = open_at + kScratchOpen.size();

        auto close_at = s.find(kScratchClose, pos);
        if (close_at == std::string::npos) {
            // Incomplete scratchpad — copy the rest and stop.
            out.append(s, pos, std::string::npos);
            break;
        }
        out.append(s, pos, close_at - pos);
        out.append(kThinkClose);
        pos = close_at + kScratchClose.size();
    }
    return out;
}

bool TrajectoryWriter::has_incomplete_scratchpad(std::string_view text) {
    if (text.empty()) return false;
    std::string s(text);
    auto open_at = s.find(kScratchOpen);
    if (open_at == std::string::npos) return false;
    return s.find(kScratchClose, open_at + kScratchOpen.size()) ==
           std::string::npos;
}

}  // namespace hermes::state
