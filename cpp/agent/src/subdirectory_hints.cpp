#include "hermes/agent/subdirectory_hints.hpp"

#include <algorithm>

namespace hermes::agent {

namespace fs = std::filesystem;

namespace {

fs::path normalise_to_dir(const fs::path& p) {
    // The caller passes the path as the agent typed it.  We treat any
    // input that has a parent_path() with content as "this is a file";
    // pure directory inputs are kept as-is.
    if (p.empty()) return p;
    std::error_code ec;
    if (fs::is_directory(p, ec)) {
        return p;
    }
    auto parent = p.parent_path();
    return parent.empty() ? p : parent;
}

}  // namespace

void SubdirectoryHintTracker::record_edit(const fs::path& path) {
    auto dir = normalise_to_dir(path);
    if (dir.empty()) return;
    const std::string key = dir.string();

    std::lock_guard<std::mutex> lock(mu_);
    auto it = index_.find(key);
    if (it != index_.end()) {
        order_.erase(it->second);
        index_.erase(it);
    }
    order_.push_front(dir);
    index_[key] = order_.begin();

    while (order_.size() > capacity_) {
        const std::string old_key = order_.back().string();
        index_.erase(old_key);
        order_.pop_back();
    }
}

std::vector<std::string> SubdirectoryHintTracker::recent(size_t n) const {
    std::lock_guard<std::mutex> lock(mu_);
    std::vector<std::string> out;
    out.reserve(std::min(n, order_.size()));
    for (const auto& p : order_) {
        if (out.size() >= n) break;
        out.push_back(p.string());
    }
    return out;
}

void SubdirectoryHintTracker::clear() {
    std::lock_guard<std::mutex> lock(mu_);
    order_.clear();
    index_.clear();
}

size_t SubdirectoryHintTracker::size() const {
    std::lock_guard<std::mutex> lock(mu_);
    return order_.size();
}

}  // namespace hermes::agent
