// TrajectoryWriter — append-only JSONL sink for agent trajectories.
//
// Mirrors agent/trajectory.py. One JSON object per line; writes are
// O_APPEND so concurrent writers within the buffer size on POSIX get
// line-level atomicity, but the class does not guarantee ordering
// across writers.
#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

namespace hermes::state {

struct TrajectoryRecord {
    std::string timestamp;         // ISO8601
    std::string model;
    nlohmann::json messages;       // OpenAI-format conversation array
    bool completed = false;
    std::optional<std::string> error;
};

class TrajectoryWriter {
public:
    // Defaults to get_hermes_home() / "trajectories" / "trajectory_samples.jsonl".
    TrajectoryWriter();
    explicit TrajectoryWriter(const std::filesystem::path& jsonl_path);

    // Append one JSON-encoded record plus a trailing newline.
    void write(const TrajectoryRecord& rec);

    // Return the resolved on-disk path (creates parent directory but
    // not the file itself).
    const std::filesystem::path& path() const { return path_; }

    // Convert <REASONING_SCRATCHPAD>...</REASONING_SCRATCHPAD> to
    // <think>...</think>. Returns `text` unchanged if no open tag is
    // found.
    static std::string convert_scratchpad_to_think(std::string_view text);

    // True iff `text` contains an opening <REASONING_SCRATCHPAD>
    // without a matching closing tag.
    static bool has_incomplete_scratchpad(std::string_view text);

private:
    std::filesystem::path path_;
};

}  // namespace hermes::state
