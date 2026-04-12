// SubdirectoryHintTracker — small LRU cache of recently-edited
// directories so the prompt builder can show "you've been working in"
// hints to the model.
#pragma once

#include <cstddef>
#include <filesystem>
#include <list>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace hermes::agent {

class SubdirectoryHintTracker {
public:
    SubdirectoryHintTracker() = default;
    explicit SubdirectoryHintTracker(size_t capacity) : capacity_(capacity) {}

    // Record that the agent has just edited `path`.  The path is
    // normalised to its parent directory and moved to the front of the
    // LRU.  Symlinks are intentionally left un-resolved (we want the
    // path the user typed, not the canonicalised one).
    void record_edit(const std::filesystem::path& path);

    // Return up to `n` most-recently-edited directories.  Order:
    // most-recent first.
    std::vector<std::string> recent(size_t n = 5) const;

    void clear();
    size_t size() const;

private:
    using Iter = std::list<std::filesystem::path>::iterator;

    mutable std::mutex mu_;
    std::list<std::filesystem::path> order_;
    std::unordered_map<std::string, Iter> index_;
    size_t capacity_ = 64;
};

}  // namespace hermes::agent
