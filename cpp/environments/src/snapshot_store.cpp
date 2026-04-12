#include "hermes/environments/snapshot_store.hpp"

#include "hermes/core/atomic_io.hpp"

#include <utility>

namespace hermes::environments {

SnapshotStore::SnapshotStore(std::filesystem::path file)
    : file_(std::move(file)) {}

nlohmann::json SnapshotStore::load_all_locked() {
    auto contents = hermes::core::atomic_io::atomic_read(file_);
    if (!contents || contents->empty()) {
        return nlohmann::json::object();
    }
    try {
        auto parsed = nlohmann::json::parse(*contents);
        if (!parsed.is_object()) {
            return nlohmann::json::object();
        }
        return parsed;
    } catch (const std::exception&) {
        // Corrupt file — start fresh.  The next save() will overwrite.
        return nlohmann::json::object();
    }
}

bool SnapshotStore::save_all_locked(const nlohmann::json& all) {
    // Ensure parent directory exists.
    auto parent = file_.parent_path();
    if (!parent.empty()) {
        std::error_code ec;
        std::filesystem::create_directories(parent, ec);
    }
    const auto serialized = all.dump(2);
    return hermes::core::atomic_io::atomic_write(file_, serialized);
}

void SnapshotStore::save(const std::string& task_id,
                         const nlohmann::json& snapshot) {
    std::lock_guard<std::mutex> lk(mu_);
    auto all = load_all_locked();
    all[task_id] = snapshot;
    save_all_locked(all);
}

std::optional<nlohmann::json> SnapshotStore::load(const std::string& task_id) {
    std::lock_guard<std::mutex> lk(mu_);
    auto all = load_all_locked();
    auto it = all.find(task_id);
    if (it == all.end()) return std::nullopt;
    return *it;
}

void SnapshotStore::remove(const std::string& task_id) {
    std::lock_guard<std::mutex> lk(mu_);
    auto all = load_all_locked();
    if (all.erase(task_id) > 0) {
        save_all_locked(all);
    }
}

}  // namespace hermes::environments
