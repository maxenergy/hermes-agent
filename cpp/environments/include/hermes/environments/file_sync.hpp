// FileSyncManager — synchronize local files to a remote environment,
// skipping unchanged files based on mtime+size.
#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace hermes::environments {

namespace fs = std::filesystem;

class FileSyncManager {
public:
    FileSyncManager() = default;

    struct FileEntry {
        fs::path local_path;
        fs::path remote_path;
    };

    // Returns the list of entries that need syncing (i.e., changed since
    // last sync based on mtime + size).
    std::vector<FileEntry> files_to_sync(
        const std::vector<FileEntry>& entries);

    // After a successful sync, call this to update the cache.
    void mark_synced(const FileEntry& entry);

    // Build a safe `rm` command that rejects paths containing `..`.
    // Returns an empty string if the path is unsafe.
    static std::string quoted_rm_command(const fs::path& remote_path);

    // Sync files to a remote using an arbitrary copy callback.
    // Returns the number of files actually transferred.
    using CopyFn = std::function<bool(const fs::path& local,
                                      const fs::path& remote)>;
    std::size_t sync_to_remote(const std::vector<FileEntry>& entries,
                               CopyFn copy_fn);

private:
    struct CacheEntry {
        std::uintmax_t size = 0;
        fs::file_time_type mtime{};
    };

    std::unordered_map<std::string, CacheEntry> cache_;
};

}  // namespace hermes::environments
