#include "hermes/environments/file_sync.hpp"

#include <algorithm>

namespace hermes::environments {

std::vector<FileSyncManager::FileEntry> FileSyncManager::files_to_sync(
    const std::vector<FileEntry>& entries) {
    std::vector<FileEntry> result;

    for (const auto& entry : entries) {
        std::error_code ec;
        auto size = fs::file_size(entry.local_path, ec);
        if (ec) {
            // File doesn't exist or can't stat — skip.
            continue;
        }
        auto mtime = fs::last_write_time(entry.local_path, ec);
        if (ec) continue;

        auto key = entry.local_path.string();
        auto it = cache_.find(key);
        if (it != cache_.end()) {
            if (it->second.size == size && it->second.mtime == mtime) {
                // Unchanged — skip.
                continue;
            }
        }

        result.push_back(entry);
    }

    return result;
}

void FileSyncManager::mark_synced(const FileEntry& entry) {
    std::error_code ec;
    auto size = fs::file_size(entry.local_path, ec);
    if (ec) return;
    auto mtime = fs::last_write_time(entry.local_path, ec);
    if (ec) return;

    cache_[entry.local_path.string()] = CacheEntry{size, mtime};
}

std::string FileSyncManager::quoted_rm_command(const fs::path& remote_path) {
    auto path_str = remote_path.string();

    // Reject paths containing ".." to prevent directory traversal.
    if (path_str.find("..") != std::string::npos) {
        return {};
    }

    // Reject empty paths.
    if (path_str.empty()) {
        return {};
    }

    return "rm -f '" + path_str + "'";
}

std::size_t FileSyncManager::sync_to_remote(
    const std::vector<FileEntry>& entries, CopyFn copy_fn) {
    auto to_sync = files_to_sync(entries);
    std::size_t transferred = 0;

    for (const auto& entry : to_sync) {
        if (copy_fn(entry.local_path, entry.remote_path)) {
            mark_synced(entry);
            ++transferred;
        }
    }

    return transferred;
}

}  // namespace hermes::environments
