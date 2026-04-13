#include "hermes/state/checkpoint_manager.hpp"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <system_error>

namespace hermes::state {

namespace {

namespace fs = std::filesystem;

// Sanitize a label or task_id so it is safe as a filename component.
// Replaces path separators + control chars with '_'.
std::string sanitize(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        unsigned char uc = static_cast<unsigned char>(c);
        if (uc < 0x20 || c == '/' || c == '\\' || c == ':' || c == '\0') {
            out.push_back('_');
        } else {
            out.push_back(c);
        }
    }
    if (out.empty()) out = "_";
    return out;
}

int64_t to_unix_ms(std::chrono::system_clock::time_point tp) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               tp.time_since_epoch()).count();
}

std::chrono::system_clock::time_point from_unix_ms(int64_t ms) {
    return std::chrono::system_clock::time_point{std::chrono::milliseconds{ms}};
}

// Recursively remove; best-effort.
void remove_all_ignore(const fs::path& p) {
    std::error_code ec;
    fs::remove_all(p, ec);
}

}  // namespace

CheckpointManager::CheckpointManager(fs::path checkpoint_root)
    : root_(std::move(checkpoint_root)) {
    std::error_code ec;
    fs::create_directories(root_, ec);
}

fs::path CheckpointManager::task_dir(const std::string& task_id) const {
    return root_ / sanitize(task_id);
}

fs::path CheckpointManager::meta_path(const std::string& task_id,
                                      const std::string& label) const {
    return task_dir(task_id) / (sanitize(label) + ".meta.json");
}

fs::path CheckpointManager::snap_path(const std::string& task_id,
                                      const std::string& label) const {
    return task_dir(task_id) / sanitize(label);
}

Checkpoint CheckpointManager::create(const std::string& task_id,
                                     const std::string& label,
                                     const fs::path& workspace,
                                     const nlohmann::json& metadata) {
    std::error_code ec;
    fs::create_directories(task_dir(task_id), ec);

    auto snap = snap_path(task_id, label);
    // Remove an existing snapshot with the same label before re-creating.
    remove_all_ignore(snap);
    fs::create_directories(snap, ec);

    if (fs::exists(workspace, ec) && fs::is_directory(workspace, ec)) {
        fs::copy(workspace, snap,
                 fs::copy_options::recursive |
                     fs::copy_options::overwrite_existing |
                     fs::copy_options::copy_symlinks,
                 ec);
    }

    Checkpoint cp;
    cp.task_id = task_id;
    cp.label = label;
    cp.snapshot_dir = snap;
    cp.created_at = std::chrono::system_clock::now();
    cp.metadata = metadata.is_null() ? nlohmann::json::object() : metadata;

    nlohmann::json sidecar;
    sidecar["task_id"] = task_id;
    sidecar["label"] = label;
    sidecar["snapshot_dir"] = snap.string();
    sidecar["created_at_ms"] = to_unix_ms(cp.created_at);
    sidecar["metadata"] = cp.metadata;

    std::ofstream ofs(meta_path(task_id, label));
    ofs << sidecar.dump(2);
    return cp;
}

std::vector<Checkpoint> CheckpointManager::list(const std::string& task_id) const {
    std::vector<Checkpoint> out;
    std::error_code ec;
    auto dir = task_dir(task_id);
    if (!fs::exists(dir, ec)) return out;
    for (const auto& ent : fs::directory_iterator(dir, ec)) {
        if (!ent.is_regular_file()) continue;
        auto p = ent.path();
        auto fname = p.filename().string();
        const std::string suffix = ".meta.json";
        if (fname.size() <= suffix.size() ||
            fname.compare(fname.size() - suffix.size(), suffix.size(), suffix) != 0) {
            continue;
        }
        std::ifstream ifs(p);
        if (!ifs) continue;
        nlohmann::json j = nlohmann::json::parse(ifs, nullptr, false);
        if (j.is_discarded()) continue;
        Checkpoint cp;
        cp.task_id = j.value("task_id", task_id);
        cp.label = j.value("label", std::string{});
        cp.snapshot_dir = j.value("snapshot_dir", std::string{});
        cp.created_at = from_unix_ms(j.value("created_at_ms", int64_t{0}));
        cp.metadata = j.value("metadata", nlohmann::json::object());
        out.push_back(std::move(cp));
    }
    std::sort(out.begin(), out.end(),
              [](const Checkpoint& a, const Checkpoint& b) {
                  return a.created_at > b.created_at;
              });
    return out;
}

std::optional<Checkpoint> CheckpointManager::get(const std::string& task_id,
                                                 const std::string& label) const {
    std::error_code ec;
    auto mp = meta_path(task_id, label);
    if (!fs::exists(mp, ec)) return std::nullopt;
    std::ifstream ifs(mp);
    if (!ifs) return std::nullopt;
    nlohmann::json j = nlohmann::json::parse(ifs, nullptr, false);
    if (j.is_discarded()) return std::nullopt;
    Checkpoint cp;
    cp.task_id = j.value("task_id", task_id);
    cp.label = j.value("label", label);
    cp.snapshot_dir = j.value("snapshot_dir", std::string{});
    cp.created_at = from_unix_ms(j.value("created_at_ms", int64_t{0}));
    cp.metadata = j.value("metadata", nlohmann::json::object());
    return cp;
}

bool CheckpointManager::restore(const std::string& task_id,
                                const std::string& label,
                                const fs::path& workspace,
                                bool overwrite) {
    auto cp = get(task_id, label);
    if (!cp) return false;
    std::error_code ec;
    if (!fs::exists(cp->snapshot_dir, ec)) return false;

    if (fs::exists(workspace, ec) && fs::is_directory(workspace, ec)) {
        bool empty = fs::is_empty(workspace, ec);
        if (!empty && !overwrite) return false;
    }
    fs::create_directories(workspace, ec);
    fs::copy(cp->snapshot_dir, workspace,
             fs::copy_options::recursive |
                 fs::copy_options::overwrite_existing |
                 fs::copy_options::copy_symlinks,
             ec);
    return !ec;
}

void CheckpointManager::remove(const std::string& task_id,
                               const std::string& label) {
    remove_all_ignore(snap_path(task_id, label));
    std::error_code ec;
    fs::remove(meta_path(task_id, label), ec);
}

void CheckpointManager::cleanup_older_than(std::chrono::hours age) {
    std::error_code ec;
    if (!fs::exists(root_, ec)) return;
    auto cutoff = std::chrono::system_clock::now() - age;
    for (const auto& task_ent : fs::directory_iterator(root_, ec)) {
        if (!task_ent.is_directory()) continue;
        auto tid = task_ent.path().filename().string();
        for (const auto& cp : list(tid)) {
            if (cp.created_at < cutoff) {
                remove(tid, cp.label);
            }
        }
    }
}

}  // namespace hermes::state
