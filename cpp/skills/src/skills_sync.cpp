#include "hermes/skills/skills_sync.hpp"

#include "hermes/skills/skill_utils.hpp"
#include "hermes/skills/skills_hub.hpp"

#include <algorithm>
#include <string>
#include <vector>

namespace hermes::skills {

namespace {

// Return 1/0/-1 comparing two dot-separated numeric version strings.
// Missing components on the shorter side are treated as zero.
int compare_version(const std::string& a, const std::string& b) {
    auto split = [](const std::string& s) {
        std::vector<int> out;
        std::string cur;
        for (char c : s) {
            if (c == '.') {
                try { out.push_back(cur.empty() ? 0 : std::stoi(cur)); }
                catch (...) { out.push_back(0); }
                cur.clear();
            } else if (c >= '0' && c <= '9') {
                cur.push_back(c);
            }
        }
        if (!cur.empty()) {
            try { out.push_back(std::stoi(cur)); } catch (...) { out.push_back(0); }
        }
        return out;
    };
    auto va = split(a);
    auto vb = split(b);
    auto n = std::max(va.size(), vb.size());
    for (std::size_t i = 0; i < n; ++i) {
        int ai = i < va.size() ? va[i] : 0;
        int bi = i < vb.size() ? vb[i] : 0;
        if (ai > bi) return 1;
        if (ai < bi) return -1;
    }
    return 0;
}

}  // namespace

SkillsSync::SkillsSync(SkillsHub* hub) : hub_(hub) {}

SyncResult SkillsSync::pull() {
    SyncResult out;
    if (!hub_) {
        out.errors.push_back("SkillsHub backend not connected");
        return out;
    }
    auto installed = iter_skill_index();
    for (const auto& local : installed) {
        auto hub_entry = hub_->get(local.name);
        if (!hub_entry) continue;  // not on hub — skip silently
        if (compare_version(hub_entry->version, local.version) > 0) {
            if (hub_->update(local.name)) {
                ++out.downloaded;
            } else {
                out.errors.push_back("update failed: " + local.name);
            }
        }
    }
    return out;
}

SyncResult SkillsSync::push(const std::string& token) {
    SyncResult out;
    if (token.empty()) {
        out.errors.push_back("auth token required to push skills");
        return out;
    }
    if (!hub_) {
        out.errors.push_back("SkillsHub backend not connected");
        return out;
    }
    // The hub upload API is not yet wired — every push is reported as an
    // error rather than a silent no-op so callers know the push failed.
    auto installed = iter_skill_index();
    for (const auto& local : installed) {
        out.errors.push_back("upload not implemented: " + local.name);
    }
    return out;
}

SyncResult SkillsSync::sync(const std::string& token) {
    SyncResult result = pull();
    auto pushed = push(token);
    result.uploaded += pushed.uploaded;
    result.conflicts += pushed.conflicts;
    for (auto& e : pushed.errors) result.errors.push_back(std::move(e));
    return result;
}

}  // namespace hermes::skills
