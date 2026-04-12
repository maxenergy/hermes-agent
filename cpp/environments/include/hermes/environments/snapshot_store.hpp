// SnapshotStore — JSON-backed persistence of cloud-sandbox identifiers.
//
// Cloud environments (Modal, Daytona, ManagedModal, …) each create a
// remote sandbox that must survive process restarts to avoid leaking
// resources.  SnapshotStore maps a user-provided `task_id` to an opaque
// JSON blob written atomically to disk, so the environment can rehydrate
// on the next run or tear down when finished.
//
// The file format is a single JSON object: `{"<task_id>": <snapshot>, …}`.
// Corrupt files are logged and treated as empty — the next successful
// `save()` rewrites them.
#pragma once

#include <filesystem>
#include <mutex>
#include <optional>
#include <string>

#include <nlohmann/json.hpp>

namespace hermes::environments {

class SnapshotStore {
public:
    explicit SnapshotStore(std::filesystem::path file);

    // Persist `snapshot` under `task_id`.  Atomic: the file is either
    // fully updated or unchanged.  Any prior entry for the same id is
    // overwritten.
    void save(const std::string& task_id, const nlohmann::json& snapshot);

    // Retrieve the snapshot for `task_id`, or std::nullopt if not found
    // or the file does not exist.
    std::optional<nlohmann::json> load(const std::string& task_id);

    // Remove the entry for `task_id`.  No-op if missing.
    void remove(const std::string& task_id);

    // Expose the path for tests.
    const std::filesystem::path& path() const { return file_; }

private:
    nlohmann::json load_all_locked();
    bool save_all_locked(const nlohmann::json& all);

    std::filesystem::path file_;
    std::mutex mu_;
};

}  // namespace hermes::environments
