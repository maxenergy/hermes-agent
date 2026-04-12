#include "hermes/state/memory_store.hpp"

#include "hermes/core/atomic_io.hpp"
#include "hermes/core/path.hpp"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <fstream>
#include <mutex>
#include <regex>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <io.h>
#include <fcntl.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif

namespace hermes::state {

namespace {

// § (U+00A7) is encoded as C2 A7 in UTF-8. The delimiter on disk is
// "\n§\n" — a lone paragraph marker surrounded by newlines so §
// characters inside an entry are preserved.
const std::string kEntryDelimiter = "\n\xc2\xa7\n";
const std::string kSectionSign = "\xc2\xa7";

std::string trim(std::string_view s) {
    std::size_t start = 0;
    while (start < s.size() && std::isspace(static_cast<unsigned char>(s[start])))
        ++start;
    std::size_t end = s.size();
    while (end > start && std::isspace(static_cast<unsigned char>(s[end - 1])))
        --end;
    return std::string(s.substr(start, end - start));
}

std::vector<std::string> split_entries(const std::string& raw) {
    std::vector<std::string> out;
    if (raw.empty()) return out;

    // We split on the full "\n§\n" delimiter. Falling back to a bare §
    // character would clobber entries that legitimately contain one.
    std::size_t pos = 0;
    while (pos < raw.size()) {
        std::size_t next = raw.find(kEntryDelimiter, pos);
        if (next == std::string::npos) {
            auto entry = trim(raw.substr(pos));
            if (!entry.empty()) out.push_back(std::move(entry));
            break;
        }
        auto entry = trim(raw.substr(pos, next - pos));
        if (!entry.empty()) out.push_back(std::move(entry));
        pos = next + kEntryDelimiter.size();
    }
    return out;
}

std::string join_entries(const std::vector<std::string>& entries) {
    if (entries.empty()) return {};
    std::string out = entries[0];
    for (std::size_t i = 1; i < entries.size(); ++i) {
        out += kEntryDelimiter;
        out += entries[i];
    }
    return out;
}

// RAII POSIX advisory file lock on a sibling .lock file.
//
// We use flock(LOCK_EX) rather than fcntl(F_SETLKW) because fcntl locks
// are process-scoped: two threads in the same process each open() their
// own fd on the .lock file and fcntl would let both acquire it because
// "the process already owns the lock." flock is per-open-file-
// description, so same-process races are correctly serialized while
// still coordinating across processes. A sibling static mutex provides
// belt-and-suspenders in-process serialization keyed on the lock path.
class FileLock {
public:
    explicit FileLock(const std::filesystem::path& path) {
        lock_path_ = path;
        lock_path_ += ".lock";
        std::error_code ec;
        std::filesystem::create_directories(lock_path_.parent_path(), ec);
        (void)ec;

        // In-process mutex keyed on the .lock path for same-process
        // thread safety.
        process_mutex_ = &process_mutex_for(lock_path_.string());
        process_mutex_->lock();

#ifdef _WIN32
        // Use LockFileEx for cross-process file locking on Windows.
        handle_ = CreateFileW(lock_path_.wstring().c_str(),
                              GENERIC_WRITE, 0, nullptr,
                              OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (handle_ != INVALID_HANDLE_VALUE) {
            OVERLAPPED ov = {};
            if (LockFileEx(handle_, LOCKFILE_EXCLUSIVE_LOCK, 0,
                           MAXDWORD, MAXDWORD, &ov)) {
                locked_ = true;
            }
        }
#else
        fd_ = ::open(lock_path_.c_str(), O_WRONLY | O_CREAT | O_CLOEXEC, 0600);
        if (fd_ < 0) return;

        if (::flock(fd_, LOCK_EX) == 0) {
            locked_ = true;
        }
#endif
    }

    ~FileLock() {
#ifdef _WIN32
        if (handle_ != INVALID_HANDLE_VALUE) {
            if (locked_) {
                OVERLAPPED ov = {};
                UnlockFileEx(handle_, 0, MAXDWORD, MAXDWORD, &ov);
            }
            CloseHandle(handle_);
        }
#else
        if (fd_ >= 0) {
            if (locked_) {
                ::flock(fd_, LOCK_UN);
            }
            ::close(fd_);
        }
#endif
        if (process_mutex_) {
            process_mutex_->unlock();
        }
    }

private:
    static std::mutex& process_mutex_for(const std::string& key) {
        static std::mutex map_mtx;
        static std::unordered_map<std::string, std::mutex> map;
        std::lock_guard<std::mutex> g(map_mtx);
        return map[key];
    }

public:

    FileLock(const FileLock&) = delete;
    FileLock& operator=(const FileLock&) = delete;

private:
    std::filesystem::path lock_path_;
    bool locked_ = false;
    std::mutex* process_mutex_ = nullptr;
#ifdef _WIN32
    HANDLE handle_ = INVALID_HANDLE_VALUE;
#else
    int fd_ = -1;
#endif
};

std::string read_file(const std::filesystem::path& p) {
    auto res = hermes::core::atomic_io::atomic_read(p);
    return res.value_or(std::string{});
}

void write_file(const std::filesystem::path& p, const std::string& content) {
    std::error_code ec;
    std::filesystem::create_directories(p.parent_path(), ec);
    (void)ec;
    hermes::core::atomic_io::atomic_write(p, content);
}

}  // namespace

MemoryStore::MemoryStore()
    : MemoryStore(hermes::core::path::get_hermes_home() / "memories") {}

MemoryStore::MemoryStore(const std::filesystem::path& memories_dir)
    : dir_(memories_dir) {
    std::error_code ec;
    std::filesystem::create_directories(dir_, ec);
    (void)ec;
}

std::filesystem::path MemoryStore::path_for(MemoryFile which) const {
    return dir_ / (which == MemoryFile::User ? "USER.md" : "MEMORY.md");
}

std::vector<std::string> MemoryStore::read_all(MemoryFile which) {
    auto p = path_for(which);
    return split_entries(read_file(p));
}

void MemoryStore::add(MemoryFile which, std::string_view entry) {
    auto trimmed = trim(entry);
    if (trimmed.empty()) return;
    auto p = path_for(which);
    FileLock lock(p);

    auto entries = split_entries(read_file(p));
    // Skip exact duplicates (matches Python semantics).
    if (std::find(entries.begin(), entries.end(), trimmed) != entries.end())
        return;
    entries.push_back(std::move(trimmed));
    write_file(p, join_entries(entries));
}

void MemoryStore::replace(MemoryFile which,
                          std::string_view needle,
                          std::string_view replacement) {
    if (needle.empty()) return;
    auto p = path_for(which);
    FileLock lock(p);

    auto entries = split_entries(read_file(p));
    for (auto& entry : entries) {
        if (entry.find(needle) != std::string::npos) {
            entry = trim(replacement);
            break;
        }
    }
    write_file(p, join_entries(entries));
}

void MemoryStore::remove(MemoryFile which, std::string_view needle) {
    if (needle.empty()) return;
    auto p = path_for(which);
    FileLock lock(p);

    auto entries = split_entries(read_file(p));
    auto it = std::find_if(entries.begin(), entries.end(),
                           [&](const std::string& e) {
                               return e.find(needle) != std::string::npos;
                           });
    if (it != entries.end()) {
        entries.erase(it);
        write_file(p, join_entries(entries));
    }
}

std::vector<MemoryStore::ThreatHit> MemoryStore::scan_for_threats(
    std::string_view content) {
    // Minimum-five patterns per spec. `std::regex` with icase.
    static const std::vector<std::pair<std::string, std::regex>> kPatterns = {
        {"prompt_injection_ignore_previous",
         std::regex(R"(ignore\s+(all\s+)?previous\s+instructions)",
                    std::regex::icase)},
        {"exfil_curl_pipe_shell",
         std::regex(R"(curl\s+\S+\s*\|\s*(sh|bash))", std::regex::icase)},
        {"ssh_key_injection",
         std::regex(R"(ssh-rsa\s+AAAA[^\s]+)")},
        {"hidden_html_div",
         std::regex(R"(<div\s+style="?display\s*:\s*none"?>)",
                    std::regex::icase)},
        {"env_file_read",
         std::regex(R"((cat|grep)\s+[^\n]*\.env)", std::regex::icase)},
    };

    std::vector<ThreatHit> hits;
    std::string s(content);
    for (const auto& [name, re] : kPatterns) {
        std::smatch m;
        if (std::regex_search(s, m, re)) {
            hits.push_back({name, m.str()});
        }
    }
    return hits;
}

}  // namespace hermes::state
