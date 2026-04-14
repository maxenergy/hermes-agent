#include <hermes/gateway/session_ttl.hpp>

#include <algorithm>
#include <sstream>

namespace hermes::gateway {

const char* compaction_reason_name(CompactionReason reason) {
    switch (reason) {
        case CompactionReason::None: return "none";
        case CompactionReason::TurnCount: return "turn-count";
        case CompactionReason::MessageCount: return "message-count";
        case CompactionReason::SizeBytes: return "size-bytes";
        case CompactionReason::Age: return "age";
        case CompactionReason::Idle: return "idle";
        case CompactionReason::Manual: return "manual";
    }
    return "unknown";
}

// --- SessionTtlTracker ---------------------------------------------------

SessionTtlTracker::SessionTtlTracker() = default;

void SessionTtlTracker::set_triggers(CompactionTriggers t) {
    std::lock_guard<std::mutex> lock(mu_);
    triggers_ = t;
}

CompactionTriggers SessionTtlTracker::triggers() const {
    std::lock_guard<std::mutex> lock(mu_);
    return triggers_;
}

void SessionTtlTracker::set_default_ttl(std::chrono::seconds ttl) {
    std::lock_guard<std::mutex> lock(mu_);
    default_ttl_ = ttl;
}

std::chrono::seconds SessionTtlTracker::default_ttl() const {
    std::lock_guard<std::mutex> lock(mu_);
    return default_ttl_;
}

void SessionTtlTracker::touch(const std::string& session_key,
                                std::chrono::system_clock::time_point now) {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = entries_.find(session_key);
    if (it == entries_.end()) {
        SessionTtlEntry e;
        e.created_at = now;
        e.last_activity = now;
        e.expires_at = now + default_ttl_;
        entries_.emplace(session_key, e);
    } else {
        it->second.last_activity = now;
    }
}

void SessionTtlTracker::record_turn(const std::string& session_key,
                                      std::size_t message_bytes) {
    auto now = std::chrono::system_clock::now();
    std::lock_guard<std::mutex> lock(mu_);
    auto it = entries_.find(session_key);
    if (it == entries_.end()) {
        SessionTtlEntry e;
        e.created_at = now;
        e.last_activity = now;
        e.expires_at = now + default_ttl_;
        e.turn_count = 1;
        e.message_count = 1;
        e.size_bytes = message_bytes;
        entries_.emplace(session_key, e);
    } else {
        auto& e = it->second;
        e.last_activity = now;
        ++e.turn_count;
        ++e.message_count;
        e.size_bytes += message_bytes;
    }
}

void SessionTtlTracker::pin(const std::string& session_key, bool pinned) {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = entries_.find(session_key);
    if (it != entries_.end()) it->second.pinned = pinned;
}

void SessionTtlTracker::extend(
    const std::string& session_key,
    std::chrono::system_clock::time_point until) {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = entries_.find(session_key);
    if (it != entries_.end()) it->second.expires_at = until;
}

std::optional<SessionTtlEntry> SessionTtlTracker::entry(
    const std::string& session_key) const {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = entries_.find(session_key);
    if (it == entries_.end()) return std::nullopt;
    return it->second;
}

std::vector<std::pair<std::string, SessionTtlEntry>>
SessionTtlTracker::entries() const {
    std::lock_guard<std::mutex> lock(mu_);
    std::vector<std::pair<std::string, SessionTtlEntry>> out(
        entries_.begin(), entries_.end());
    std::sort(out.begin(), out.end(),
               [](const auto& a, const auto& b) { return a.first < b.first; });
    return out;
}

std::vector<std::string> SessionTtlTracker::expired(
    std::chrono::system_clock::time_point now) const {
    std::lock_guard<std::mutex> lock(mu_);
    std::vector<std::string> out;
    for (auto& [k, v] : entries_) {
        if (v.pinned) continue;
        if (v.expires_at <= now) out.push_back(k);
    }
    std::sort(out.begin(), out.end());
    return out;
}

std::vector<std::pair<std::string, CompactionReason>>
SessionTtlTracker::due_for_compaction(
    std::chrono::system_clock::time_point now) const {
    std::lock_guard<std::mutex> lock(mu_);
    std::vector<std::pair<std::string, CompactionReason>> out;
    for (auto& [k, v] : entries_) {
        if (v.pinned) continue;
        CompactionReason r = CompactionReason::None;
        if (v.turn_count >= triggers_.max_turns) r = CompactionReason::TurnCount;
        else if (v.message_count >= triggers_.max_messages)
            r = CompactionReason::MessageCount;
        else if (v.size_bytes >= triggers_.max_bytes)
            r = CompactionReason::SizeBytes;
        else if (now - v.created_at >= triggers_.max_age)
            r = CompactionReason::Age;
        else if (now - v.last_activity >= triggers_.max_idle)
            r = CompactionReason::Idle;
        if (r != CompactionReason::None) out.emplace_back(k, r);
    }
    std::sort(out.begin(), out.end(),
               [](const auto& a, const auto& b) { return a.first < b.first; });
    return out;
}

void SessionTtlTracker::forget(const std::string& session_key) {
    std::lock_guard<std::mutex> lock(mu_);
    entries_.erase(session_key);
}

std::size_t SessionTtlTracker::size() const {
    std::lock_guard<std::mutex> lock(mu_);
    return entries_.size();
}

std::size_t SessionTtlTracker::total_bytes() const {
    std::lock_guard<std::mutex> lock(mu_);
    std::size_t n = 0;
    for (auto& [_, v] : entries_) n += v.size_bytes;
    return n;
}

// --- InsightLedger -------------------------------------------------------

InsightLedger::InsightLedger() = default;

void InsightLedger::record(SessionInsight insight) {
    std::lock_guard<std::mutex> lock(mu_);
    if (insight.recorded_at.time_since_epoch().count() == 0) {
        insight.recorded_at = std::chrono::system_clock::now();
    }
    ledger_.push_back(std::move(insight));
}

std::vector<SessionInsight> InsightLedger::for_session(
    const std::string& session_key) const {
    std::lock_guard<std::mutex> lock(mu_);
    std::vector<SessionInsight> out;
    for (auto& i : ledger_)
        if (i.session_key == session_key) out.push_back(i);
    return out;
}

std::vector<SessionInsight> InsightLedger::by_category(
    const std::string& category) const {
    std::lock_guard<std::mutex> lock(mu_);
    std::vector<SessionInsight> out;
    for (auto& i : ledger_)
        if (i.category == category) out.push_back(i);
    return out;
}

std::vector<SessionInsight> InsightLedger::top_for_session(
    const std::string& session_key, std::size_t n) const {
    auto out = for_session(session_key);
    std::sort(out.begin(), out.end(),
               [](const SessionInsight& a, const SessionInsight& b) {
                   return a.score > b.score;
               });
    if (out.size() > n) out.resize(n);
    return out;
}

std::size_t InsightLedger::purge_session(const std::string& session_key) {
    std::lock_guard<std::mutex> lock(mu_);
    auto before = ledger_.size();
    ledger_.erase(std::remove_if(ledger_.begin(), ledger_.end(),
                                   [&](const SessionInsight& i) {
                                       return i.session_key == session_key;
                                   }),
                   ledger_.end());
    return before - ledger_.size();
}

std::size_t InsightLedger::size() const {
    std::lock_guard<std::mutex> lock(mu_);
    return ledger_.size();
}

std::vector<std::string> InsightLedger::categories() const {
    std::lock_guard<std::mutex> lock(mu_);
    std::vector<std::string> out;
    for (auto& i : ledger_) out.push_back(i.category);
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

void InsightLedger::clear() {
    std::lock_guard<std::mutex> lock(mu_);
    ledger_.clear();
}

// --- SessionLinker -------------------------------------------------------

namespace {

std::string composite_key(Platform p, const std::string& id) {
    return platform_to_string(p) + ":" + id;
}

}  // namespace

SessionLinker::SessionLinker() = default;

std::string SessionLinker::link(Platform platform,
                                  const std::string& platform_user_id,
                                  std::string canonical_id) {
    std::lock_guard<std::mutex> lock(mu_);
    auto key = composite_key(platform, platform_user_id);
    auto existing = by_composite_.find(key);
    if (existing != by_composite_.end()) return existing->second;

    if (canonical_id.empty()) {
        std::ostringstream os;
        os << "canon-" << (++counter_);
        canonical_id = os.str();
    }

    by_composite_[key] = canonical_id;
    by_canonical_[canonical_id].emplace_back(platform, platform_user_id);
    return canonical_id;
}

bool SessionLinker::unlink(Platform platform,
                              const std::string& platform_user_id) {
    std::lock_guard<std::mutex> lock(mu_);
    auto key = composite_key(platform, platform_user_id);
    auto it = by_composite_.find(key);
    if (it == by_composite_.end()) return false;
    auto canon = it->second;
    by_composite_.erase(it);
    auto jt = by_canonical_.find(canon);
    if (jt != by_canonical_.end()) {
        auto& v = jt->second;
        v.erase(std::remove_if(v.begin(), v.end(),
                                 [&](const std::pair<Platform, std::string>& p) {
                                     return p.first == platform &&
                                            p.second == platform_user_id;
                                 }),
                v.end());
        if (v.empty()) by_canonical_.erase(jt);
    }
    return true;
}

std::optional<std::string> SessionLinker::canonical_for(
    Platform platform, const std::string& platform_user_id) const {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = by_composite_.find(composite_key(platform, platform_user_id));
    if (it == by_composite_.end()) return std::nullopt;
    return it->second;
}

std::vector<std::pair<Platform, std::string>>
SessionLinker::identities_for(const std::string& canonical_id) const {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = by_canonical_.find(canonical_id);
    if (it == by_canonical_.end()) return {};
    return it->second;
}

std::size_t SessionLinker::canonical_count() const {
    std::lock_guard<std::mutex> lock(mu_);
    return by_canonical_.size();
}

std::size_t SessionLinker::identity_count() const {
    std::lock_guard<std::mutex> lock(mu_);
    return by_composite_.size();
}

void SessionLinker::clear() {
    std::lock_guard<std::mutex> lock(mu_);
    by_composite_.clear();
    by_canonical_.clear();
    counter_ = 0;
}

}  // namespace hermes::gateway
