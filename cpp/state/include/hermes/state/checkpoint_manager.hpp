// Phase 12: Long-task checkpoint manager.
//
// Snapshots a workspace directory (recursive copy) and records free-form
// JSON metadata so a long-running agent task can be rolled back to an
// earlier known-good state.  Sidecar metadata lives at
// ``<root>/<task_id>/<label>.meta.json``; snapshot files at
// ``<root>/<task_id>/<label>/...``.
#pragma once

#include <chrono>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace hermes::state {

struct Checkpoint {
    std::string task_id;
    std::string label;
    std::filesystem::path snapshot_dir;
    std::chrono::system_clock::time_point created_at{};
    nlohmann::json metadata;
};

class CheckpointManager {
public:
    explicit CheckpointManager(std::filesystem::path checkpoint_root);

    /// Snapshot the given workspace dir + record metadata.
    Checkpoint create(const std::string& task_id,
                      const std::string& label,
                      const std::filesystem::path& workspace,
                      const nlohmann::json& metadata = {});

    /// List all checkpoints for the given task_id, newest first.
    std::vector<Checkpoint> list(const std::string& task_id) const;

    /// Get the checkpoint with the given label, if it exists.
    std::optional<Checkpoint> get(const std::string& task_id,
                                  const std::string& label) const;

    /// Restore: copy snapshot back into workspace. Requires overwrite=true
    /// when the workspace is non-empty — otherwise returns false.
    bool restore(const std::string& task_id,
                 const std::string& label,
                 const std::filesystem::path& workspace,
                 bool overwrite = false);

    /// Delete the checkpoint snapshot + sidecar.
    void remove(const std::string& task_id, const std::string& label);

    /// Delete every checkpoint older than ``age``.
    void cleanup_older_than(std::chrono::hours age);

    const std::filesystem::path& root() const { return root_; }

private:
    std::filesystem::path root_;

    std::filesystem::path task_dir(const std::string& task_id) const;
    std::filesystem::path meta_path(const std::string& task_id,
                                    const std::string& label) const;
    std::filesystem::path snap_path(const std::string& task_id,
                                    const std::string& label) const;
};

}  // namespace hermes::state
