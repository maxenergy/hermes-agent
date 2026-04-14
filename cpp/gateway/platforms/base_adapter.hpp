// Shared scaffolding for every BasePlatformAdapter implementation.
//
// Ports the depth of gateway/platforms/base.py's BasePlatformAdapter:
//
//   - Connected / disconnected state transitions (with fatal-error flag)
//   - Rate-limit state machine (token bucket + leaky cooldown)
//   - Retry budget (ring buffer of recent failures)
//   - Feature-flags table (per-adapter capability map)
//   - Health-probe contract (liveness + readiness)
//   - Typing-indicator pause/resume bookkeeping
//   - Standardized AdapterErrorKind classification
//
// Every concrete adapter (Telegram, Discord, …) extends
// ``BaseAdapterMixin`` alongside its own ``BasePlatformAdapter`` subclass
// to gain these facilities without rewriting them.  The mixin is
// header-mostly because the state is per-instance and POD-like.
#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <hermes/gateway/gateway_runner.hpp>  // AdapterErrorKind

namespace hermes::gateway {

// --- Rate-limit state machine -------------------------------------------

// A simple token bucket with leak rate ``refill_per_sec`` and capacity
// ``burst``.  Thread-safe.  Returns ``Retry`` to tell the adapter to
// pause; the ``retry_after`` field carries the suggested delay.
class TokenBucket {
public:
    TokenBucket() = default;
    TokenBucket(double refill_per_sec, double burst);

    // Reconfigure at runtime (e.g., after receiving a rate-limit
    // response).  Safe to call from any thread.
    void reconfigure(double refill_per_sec, double burst);

    // Attempt to consume ``n`` tokens.  Returns true on success; if
    // false, ``retry_after`` is populated with the wait needed to
    // accumulate ``n`` tokens.
    bool try_consume(double n,
                      std::chrono::milliseconds* retry_after = nullptr);

    // Force the bucket into a cooldown until ``until`` — subsequent
    // ``try_consume`` returns false until then regardless of token
    // count.  Used when the platform returns an explicit retry-after
    // hint.
    void cooldown_until(std::chrono::steady_clock::time_point until);

    // Current number of tokens (approximate — not locked).
    double tokens() const;

    // Reset the bucket to full capacity.
    void refill_full();

private:
    mutable std::mutex mu_;
    double refill_ = 1.0;
    double burst_ = 1.0;
    double level_ = 1.0;
    std::chrono::steady_clock::time_point last_refill_{};
    std::optional<std::chrono::steady_clock::time_point> cooldown_;
};

// --- Retry budget --------------------------------------------------------

// Tracks the last ``window`` failures for the adapter.  When the window
// is full of retryable failures, the adapter is considered "budget
// exhausted" and the reconnect watcher stops retrying until the user
// takes manual action.
class RetryBudget {
public:
    explicit RetryBudget(std::size_t window = 10,
                          std::chrono::seconds reset_after = std::chrono::seconds(600));

    // Record an outcome; pass ``true`` for a retryable failure, ``false``
    // for a success.  Success clears the window.
    void record(bool is_retryable_failure);

    // Record an explicit fatal failure — the adapter should not retry.
    void record_fatal();

    // Current number of consecutive retryable failures (post-reset).
    std::size_t failure_count() const;

    // True once the window is full of retryable failures.
    bool exhausted() const;

    // True once a fatal error has been recorded (until ``clear``).
    bool fatal() const;

    // Clear all state (re-enable retries).
    void clear();

    // Window size — immutable.
    std::size_t window_size() const { return window_; }

    // Compute the adjusted backoff for the current failure count,
    // capped at ``max``.  Uses jittered exponential backoff.
    std::chrono::milliseconds next_backoff(
        std::chrono::milliseconds base,
        std::chrono::milliseconds max) const;

private:
    mutable std::mutex mu_;
    std::deque<std::chrono::steady_clock::time_point> failures_;
    std::size_t window_;
    std::chrono::seconds reset_after_;
    bool fatal_ = false;
};

// --- Feature flags -------------------------------------------------------

// Per-adapter capability table.  Each feature has a name and a current
// state (Enabled/Disabled/Unsupported).  Adapters populate the initial
// map at construction; the runner can toggle individual features via
// ``set_feature`` (e.g., disable ``voice`` when libopus is absent).
enum class FeatureState {
    Enabled,
    Disabled,
    Unsupported,
};

class FeatureFlags {
public:
    FeatureFlags() = default;

    // Register a feature with an initial state.  Returns false if a
    // feature with the same name is already registered.
    bool register_feature(std::string name, FeatureState state);

    // Change the state of an already-registered feature.
    bool set_state(const std::string& name, FeatureState state);

    FeatureState state(const std::string& name) const;

    // Shortcut — ``is_enabled("voice")``.
    bool is_enabled(const std::string& name) const;

    std::vector<std::string> known_features() const;

    // Convenience: dump all features into a (name -> state) table.
    std::unordered_map<std::string, FeatureState> snapshot() const;

private:
    mutable std::mutex mu_;
    std::unordered_map<std::string, FeatureState> features_;
};

// --- Health probe --------------------------------------------------------

// Result of a single health check.
struct HealthSnapshot {
    bool connected = false;
    bool ready = false;         // adapter can accept traffic
    std::string last_error;     // last error message, if any
    AdapterErrorKind last_error_kind = AdapterErrorKind::None;
    std::chrono::system_clock::time_point last_success{};
    std::chrono::system_clock::time_point last_failure{};
    std::size_t failure_streak = 0;
    double tokens_available = 0.0;
};

// --- BaseAdapterMixin ----------------------------------------------------

// Stateful backbone.  Each adapter composes a single instance of
// ``BaseAdapterMixin`` (typically as a private member) and exposes the
// pieces it cares about through the ``BasePlatformAdapter`` interface.
class BaseAdapterMixin {
public:
    BaseAdapterMixin();

    // --- State transitions ----------------------------------------------
    void mark_connected();
    void mark_disconnected();
    bool is_connected() const;

    void set_fatal_error(std::string code, std::string message,
                          bool retryable = false);
    void clear_fatal_error();
    bool has_fatal_error() const;
    std::optional<std::string> fatal_error_code() const;
    std::optional<std::string> fatal_error_message() const;
    bool fatal_error_retryable() const;

    // --- Error classification -------------------------------------------
    void record_error(AdapterErrorKind kind, std::string message = {});
    AdapterErrorKind last_error_kind() const;
    std::string last_error_message() const;

    // Classify a raw error text using keywords that match the Python
    // ``_is_retryable_error`` / ``_is_timeout_error`` heuristics.
    static AdapterErrorKind classify_error(std::string_view text);

    // --- Rate limit / retry budget / features ---------------------------
    TokenBucket& rate_limit() { return bucket_; }
    const TokenBucket& rate_limit() const { return bucket_; }
    RetryBudget& retry_budget() { return budget_; }
    const RetryBudget& retry_budget() const { return budget_; }
    FeatureFlags& features() { return flags_; }
    const FeatureFlags& features() const { return flags_; }

    // --- Typing indicator pause ----------------------------------------
    void pause_typing_for_chat(const std::string& chat_id);
    void resume_typing_for_chat(const std::string& chat_id);
    bool is_typing_paused(const std::string& chat_id) const;

    // --- Health probe ---------------------------------------------------
    HealthSnapshot snapshot_health() const;
    void on_send_success();
    void on_send_failure(AdapterErrorKind kind, const std::string& msg);

private:
    mutable std::mutex mu_;
    std::atomic<bool> connected_{false};

    bool fatal_ = false;
    std::string fatal_code_;
    std::string fatal_message_;
    bool fatal_retryable_ = false;

    AdapterErrorKind last_kind_ = AdapterErrorKind::None;
    std::string last_error_message_;
    std::chrono::system_clock::time_point last_success_{};
    std::chrono::system_clock::time_point last_failure_{};
    std::size_t failure_streak_ = 0;

    TokenBucket bucket_{1.0, 1.0};
    RetryBudget budget_{10, std::chrono::seconds(600)};
    FeatureFlags flags_;

    std::unordered_set<std::string> typing_paused_;
};

}  // namespace hermes::gateway
