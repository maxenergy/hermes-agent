#include "base_adapter.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <random>
#include <string>

namespace hermes::gateway {

namespace {

std::string lowercase(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s)
        out.push_back(static_cast<char>(std::tolower(
            static_cast<unsigned char>(c))));
    return out;
}

bool contains(const std::string& hay, const char* needle) {
    return hay.find(needle) != std::string::npos;
}

}  // namespace

// --- TokenBucket ---------------------------------------------------------

TokenBucket::TokenBucket(double refill_per_sec, double burst)
    : refill_(refill_per_sec), burst_(burst), level_(burst),
      last_refill_(std::chrono::steady_clock::now()) {}

void TokenBucket::reconfigure(double refill_per_sec, double burst) {
    std::lock_guard<std::mutex> lock(mu_);
    refill_ = std::max(0.0, refill_per_sec);
    burst_ = std::max(0.1, burst);
    level_ = std::min(level_, burst_);
    if (level_ < 0) level_ = 0;
    last_refill_ = std::chrono::steady_clock::now();
}

bool TokenBucket::try_consume(double n,
                                std::chrono::milliseconds* retry_after) {
    std::lock_guard<std::mutex> lock(mu_);
    auto now = std::chrono::steady_clock::now();

    // Refill.
    auto elapsed = std::chrono::duration<double>(now - last_refill_).count();
    last_refill_ = now;
    level_ = std::min(burst_, level_ + elapsed * refill_);

    // Cooldown?
    if (cooldown_.has_value() && now < *cooldown_) {
        if (retry_after) {
            *retry_after = std::chrono::duration_cast<std::chrono::milliseconds>(
                *cooldown_ - now);
        }
        return false;
    }
    cooldown_.reset();

    if (level_ >= n) {
        level_ -= n;
        return true;
    }

    if (retry_after) {
        double deficit = n - level_;
        double secs = refill_ > 0.0 ? (deficit / refill_) : 60.0;
        *retry_after =
            std::chrono::milliseconds(static_cast<long long>(secs * 1000.0));
    }
    return false;
}

void TokenBucket::cooldown_until(
    std::chrono::steady_clock::time_point until) {
    std::lock_guard<std::mutex> lock(mu_);
    cooldown_ = until;
}

double TokenBucket::tokens() const {
    std::lock_guard<std::mutex> lock(mu_);
    return level_;
}

void TokenBucket::refill_full() {
    std::lock_guard<std::mutex> lock(mu_);
    level_ = burst_;
    cooldown_.reset();
    last_refill_ = std::chrono::steady_clock::now();
}

// --- RetryBudget ---------------------------------------------------------

RetryBudget::RetryBudget(std::size_t window, std::chrono::seconds reset_after)
    : window_(window), reset_after_(reset_after) {}

void RetryBudget::record(bool is_retryable_failure) {
    std::lock_guard<std::mutex> lock(mu_);
    auto now = std::chrono::steady_clock::now();
    // Evict stale entries.
    while (!failures_.empty() && now - failures_.front() > reset_after_) {
        failures_.pop_front();
    }
    if (!is_retryable_failure) {
        failures_.clear();
        return;
    }
    failures_.push_back(now);
    while (failures_.size() > window_) failures_.pop_front();
}

void RetryBudget::record_fatal() {
    std::lock_guard<std::mutex> lock(mu_);
    fatal_ = true;
}

std::size_t RetryBudget::failure_count() const {
    std::lock_guard<std::mutex> lock(mu_);
    auto now = std::chrono::steady_clock::now();
    std::size_t n = 0;
    for (auto& t : failures_) {
        if (now - t <= reset_after_) ++n;
    }
    return n;
}

bool RetryBudget::exhausted() const {
    return failure_count() >= window_;
}

bool RetryBudget::fatal() const {
    std::lock_guard<std::mutex> lock(mu_);
    return fatal_;
}

void RetryBudget::clear() {
    std::lock_guard<std::mutex> lock(mu_);
    failures_.clear();
    fatal_ = false;
}

std::chrono::milliseconds RetryBudget::next_backoff(
    std::chrono::milliseconds base, std::chrono::milliseconds max) const {
    auto n = failure_count();
    if (n == 0) return base;
    // 2^(n-1) with jitter.
    double pow2 = std::pow(2.0, static_cast<double>(n - 1));
    auto raw = std::chrono::milliseconds(
        static_cast<long long>(base.count() * pow2));
    if (raw > max) raw = max;
    // Deterministic pseudo-jitter seeded by failure count.
    std::mt19937 rng(static_cast<std::uint32_t>(n * 2654435761u));
    std::uniform_real_distribution<double> dist(0.8, 1.2);
    auto jittered = std::chrono::milliseconds(
        static_cast<long long>(raw.count() * dist(rng)));
    if (jittered > max) jittered = max;
    return jittered;
}

// --- FeatureFlags --------------------------------------------------------

bool FeatureFlags::register_feature(std::string name, FeatureState state) {
    std::lock_guard<std::mutex> lock(mu_);
    auto [it, inserted] = features_.emplace(std::move(name), state);
    (void)it;
    return inserted;
}

bool FeatureFlags::set_state(const std::string& name, FeatureState state) {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = features_.find(name);
    if (it == features_.end()) return false;
    it->second = state;
    return true;
}

FeatureState FeatureFlags::state(const std::string& name) const {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = features_.find(name);
    if (it == features_.end()) return FeatureState::Unsupported;
    return it->second;
}

bool FeatureFlags::is_enabled(const std::string& name) const {
    return state(name) == FeatureState::Enabled;
}

std::vector<std::string> FeatureFlags::known_features() const {
    std::lock_guard<std::mutex> lock(mu_);
    std::vector<std::string> out;
    out.reserve(features_.size());
    for (auto& [k, _] : features_) out.push_back(k);
    std::sort(out.begin(), out.end());
    return out;
}

std::unordered_map<std::string, FeatureState>
FeatureFlags::snapshot() const {
    std::lock_guard<std::mutex> lock(mu_);
    return features_;
}

// --- BaseAdapterMixin ---------------------------------------------------

BaseAdapterMixin::BaseAdapterMixin() = default;

void BaseAdapterMixin::mark_connected() {
    connected_.store(true, std::memory_order_release);
    std::lock_guard<std::mutex> lock(mu_);
    fatal_ = false;
    fatal_code_.clear();
    fatal_message_.clear();
    last_kind_ = AdapterErrorKind::None;
    last_error_message_.clear();
}

void BaseAdapterMixin::mark_disconnected() {
    connected_.store(false, std::memory_order_release);
}

bool BaseAdapterMixin::is_connected() const {
    return connected_.load(std::memory_order_acquire);
}

void BaseAdapterMixin::set_fatal_error(std::string code, std::string message,
                                         bool retryable) {
    std::lock_guard<std::mutex> lock(mu_);
    fatal_ = true;
    fatal_code_ = std::move(code);
    fatal_message_ = std::move(message);
    fatal_retryable_ = retryable;
    last_kind_ = retryable ? AdapterErrorKind::Retryable
                            : AdapterErrorKind::Fatal;
    last_error_message_ = fatal_message_;
}

void BaseAdapterMixin::clear_fatal_error() {
    std::lock_guard<std::mutex> lock(mu_);
    fatal_ = false;
    fatal_code_.clear();
    fatal_message_.clear();
    fatal_retryable_ = false;
}

bool BaseAdapterMixin::has_fatal_error() const {
    std::lock_guard<std::mutex> lock(mu_);
    return fatal_;
}

std::optional<std::string> BaseAdapterMixin::fatal_error_code() const {
    std::lock_guard<std::mutex> lock(mu_);
    if (!fatal_) return std::nullopt;
    return fatal_code_;
}

std::optional<std::string> BaseAdapterMixin::fatal_error_message() const {
    std::lock_guard<std::mutex> lock(mu_);
    if (!fatal_) return std::nullopt;
    return fatal_message_;
}

bool BaseAdapterMixin::fatal_error_retryable() const {
    std::lock_guard<std::mutex> lock(mu_);
    return fatal_ && fatal_retryable_;
}

void BaseAdapterMixin::record_error(AdapterErrorKind kind,
                                      std::string message) {
    std::lock_guard<std::mutex> lock(mu_);
    last_kind_ = kind;
    last_error_message_ = std::move(message);
    last_failure_ = std::chrono::system_clock::now();
    ++failure_streak_;
}

AdapterErrorKind BaseAdapterMixin::last_error_kind() const {
    std::lock_guard<std::mutex> lock(mu_);
    return last_kind_;
}

std::string BaseAdapterMixin::last_error_message() const {
    std::lock_guard<std::mutex> lock(mu_);
    return last_error_message_;
}

AdapterErrorKind BaseAdapterMixin::classify_error(std::string_view text) {
    if (text.empty()) return AdapterErrorKind::Unknown;
    auto lower = lowercase(text);

    // Fatal auth / permission signals first.
    if (contains(lower, "unauthorized") || contains(lower, "401") ||
        contains(lower, "forbidden") || contains(lower, "403") ||
        contains(lower, "invalid token") || contains(lower, "bad credentials") ||
        contains(lower, "revoked")) {
        return AdapterErrorKind::Fatal;
    }
    if (contains(lower, "lock conflict") || contains(lower, "already in use")) {
        return AdapterErrorKind::Fatal;
    }

    // Retryable signals.
    if (contains(lower, "timeout") || contains(lower, "timed out") ||
        contains(lower, "temporarily unavailable") ||
        contains(lower, "429") || contains(lower, "rate limit") ||
        contains(lower, "retry-after") || contains(lower, "try again") ||
        contains(lower, "503") || contains(lower, "502") ||
        contains(lower, "504") || contains(lower, "500") ||
        contains(lower, "network") || contains(lower, "connection reset") ||
        contains(lower, "ecnnreset") || contains(lower, "connection refused")) {
        return AdapterErrorKind::Retryable;
    }

    return AdapterErrorKind::Unknown;
}

void BaseAdapterMixin::pause_typing_for_chat(const std::string& chat_id) {
    std::lock_guard<std::mutex> lock(mu_);
    typing_paused_.insert(chat_id);
}

void BaseAdapterMixin::resume_typing_for_chat(const std::string& chat_id) {
    std::lock_guard<std::mutex> lock(mu_);
    typing_paused_.erase(chat_id);
}

bool BaseAdapterMixin::is_typing_paused(const std::string& chat_id) const {
    std::lock_guard<std::mutex> lock(mu_);
    return typing_paused_.count(chat_id) > 0;
}

HealthSnapshot BaseAdapterMixin::snapshot_health() const {
    HealthSnapshot s;
    s.connected = connected_.load(std::memory_order_acquire);
    std::lock_guard<std::mutex> lock(mu_);
    s.ready = s.connected && !fatal_;
    s.last_error = last_error_message_;
    s.last_error_kind = last_kind_;
    s.last_success = last_success_;
    s.last_failure = last_failure_;
    s.failure_streak = failure_streak_;
    s.tokens_available = bucket_.tokens();
    return s;
}

void BaseAdapterMixin::on_send_success() {
    {
        std::lock_guard<std::mutex> lock(mu_);
        last_success_ = std::chrono::system_clock::now();
        failure_streak_ = 0;
        last_kind_ = AdapterErrorKind::None;
    }
    budget_.record(false);
}

void BaseAdapterMixin::on_send_failure(AdapterErrorKind kind,
                                         const std::string& msg) {
    record_error(kind, msg);
    if (kind == AdapterErrorKind::Fatal) {
        budget_.record_fatal();
    } else {
        budget_.record(true);
    }
}

}  // namespace hermes::gateway
