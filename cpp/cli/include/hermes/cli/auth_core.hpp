// C++17 port of the foundational primitives from hermes_cli/auth.py.
//
// The full auth module is ~3100 LoC and spans provider discovery, OAuth
// flows, managed-subscription probes, and credential-pool orchestration.
// This header covers the stable foundation that every auth call path
// needs:
//
//   * AuthError — structured error with UX mapping hints.
//   * Token / JWT / ISO-8601 helpers used for expiry tracking.
//   * The ``~/.hermes/auth.json`` store — load / save with cross-process
//     advisory locking, provider-slot accessors, active-provider state,
//     credential-pool slices, and suppressed-source bookkeeping.
//   * ``ProviderConfig`` + registry — env-var names, auth-type, base URL,
//     managed-gateway flags.
//   * ``is_provider_explicitly_configured`` — the gate used to avoid
//     picking up external credentials without user consent.
//
// More specialised helpers (OAuth device-code runners, provider-specific
// token refresh) layer on top of these primitives in subsequent files.
#pragma once

#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace hermes::cli::auth_core {

// ---------------------------------------------------------------------------
// Constants.
// ---------------------------------------------------------------------------

inline constexpr int kAuthLockTimeoutSeconds = 30;
inline constexpr int kDefaultExpirySkewSeconds = 60;

// Auth-type tags recognised in auth.json and config.yaml.
inline constexpr const char* kAuthTypeApiKey = "api_key";
inline constexpr const char* kAuthTypeOAuth = "oauth";
inline constexpr const char* kAuthTypeManaged = "managed";

// Env var names that are set implicitly by external tooling and must NOT
// count as "user explicitly configured" this provider.
const std::unordered_set<std::string>& implicit_env_vars();

// ---------------------------------------------------------------------------
// AuthError — structured exception with UX mapping hints.
// ---------------------------------------------------------------------------

class AuthError : public std::runtime_error {
public:
    AuthError(const std::string& message,
              const std::string& provider = {},
              const std::optional<std::string>& code = std::nullopt,
              bool relogin_required = false);

    const std::string& provider() const { return provider_; }
    const std::optional<std::string>& code() const { return code_; }
    bool relogin_required() const { return relogin_required_; }

private:
    std::string provider_;
    std::optional<std::string> code_;
    bool relogin_required_ = false;
};

// Map auth failures to concise user-facing guidance.  Unknown exception
// types fall back to ``what()``.
std::string format_auth_error(const std::exception& error);

// ---------------------------------------------------------------------------
// Token / JWT / ISO-8601 helpers.
// ---------------------------------------------------------------------------

// Return a 12-char SHA-256 hex fingerprint for telemetry without leaking
// the secret.  Empty/non-string inputs return nullopt.
std::optional<std::string> token_fingerprint(const std::string& token);

// Return true when HERMES_OAUTH_TRACE is set to a truthy value.
bool oauth_trace_enabled();

// Check whether a value is a non-empty secret of at least ``min_length``
// characters (trimmed).  Anything non-string returns false.
bool has_usable_secret(const nlohmann::json& value, std::size_t min_length = 4);
bool has_usable_secret(const std::string& value, std::size_t min_length = 4);

// Parse an ISO-8601 timestamp (with optional trailing ``Z``) and return
// the epoch seconds.  Returns nullopt on any failure.
std::optional<double> parse_iso_timestamp(const std::string& value);
std::optional<double> parse_iso_timestamp(const nlohmann::json& value);

// True when ``expires_at_iso`` has already lapsed or will within
// ``skew_seconds``.  Missing / un-parseable values return true (treated
// as expiring — forces a refresh).
bool is_expiring(const nlohmann::json& expires_at_iso,
                 int skew_seconds = kDefaultExpirySkewSeconds);

// Coerce an ``expires_in`` value (seconds) to a non-negative integer.
int coerce_ttl_seconds(const nlohmann::json& expires_in);

// Trim + strip trailing '/' from a base URL.  Empty / non-string → nullopt.
std::optional<std::string> optional_base_url(const nlohmann::json& value);

// Decode the payload segment of a JWT.  Returns an empty object on any
// failure (malformed structure, invalid base64, non-JSON payload).
nlohmann::json decode_jwt_claims(const std::string& token);

// True when a Codex-style access token's ``exp`` claim falls within the
// skew window.  Missing claims → false (do not force refresh).
bool codex_access_token_is_expiring(const std::string& access_token,
                                    int skew_seconds);

// ---------------------------------------------------------------------------
// Auth store — ~/.hermes/auth.json persistence.
// ---------------------------------------------------------------------------

std::filesystem::path auth_file_path();
std::filesystem::path auth_lock_path();

// Acquire a cross-process advisory lock using flock(2).  The returned
// opaque handle releases the lock when destroyed.  Reentrant from the
// same process via a thread-local counter.
class AuthStoreLock {
public:
    explicit AuthStoreLock(double timeout_seconds = kAuthLockTimeoutSeconds);
    ~AuthStoreLock();
    AuthStoreLock(const AuthStoreLock&) = delete;
    AuthStoreLock& operator=(const AuthStoreLock&) = delete;
    bool acquired() const { return acquired_; }

private:
    int fd_ = -1;
    bool acquired_ = false;
    bool reentered_ = false;
};

// Load the auth store JSON.  Missing or malformed files yield an empty
// object.  Not locked — callers that mutate should take the lock first.
nlohmann::json load_auth_store();
nlohmann::json load_auth_store(const std::filesystem::path& path);

// Atomically write the auth store.  Returns the path written to.
std::filesystem::path save_auth_store(const nlohmann::json& auth_store);
std::filesystem::path save_auth_store(const nlohmann::json& auth_store,
                                      const std::filesystem::path& path);

// Provider state slot accessors.
std::optional<nlohmann::json> load_provider_state(
    const nlohmann::json& auth_store, const std::string& provider_id);
void save_provider_state(nlohmann::json& auth_store,
                         const std::string& provider_id,
                         const nlohmann::json& state);

// ---------------------------------------------------------------------------
// Active provider tracking.
// ---------------------------------------------------------------------------

std::optional<std::string> get_active_provider();

// Returns auth state for a provider, or nullopt if absent.
std::optional<nlohmann::json> get_provider_auth_state(
    const std::string& provider_id);

// Clear a specific provider (or all providers when empty).  Returns true
// if anything was removed.
bool clear_provider_auth(const std::string& provider_id = {});

// Clear the active provider pointer without touching per-provider state.
void deactivate_provider();

// ---------------------------------------------------------------------------
// Credential pool helpers.
// ---------------------------------------------------------------------------

// Read the credential pool (all providers).  Returns an object mapping
// provider_id → list of entries.
nlohmann::json read_credential_pool();

// Read one provider's slice; returns a JSON array.
nlohmann::json read_credential_pool(const std::string& provider_id);

// Persist one provider's pool.  Returns the path written to.
std::filesystem::path write_credential_pool(
    const std::string& provider_id,
    const std::vector<nlohmann::json>& entries);

// Mark a credential source as suppressed so it won't be re-seeded.
void suppress_credential_source(const std::string& provider_id,
                                const std::string& source);

bool is_source_suppressed(const std::string& provider_id,
                          const std::string& source);

// ---------------------------------------------------------------------------
// Provider registry — subset of hermes_cli/auth.py PROVIDER_REGISTRY.
// ---------------------------------------------------------------------------

struct ProviderConfig {
    std::string id;
    std::string display_name;
    std::string auth_type;  // "api_key" / "oauth" / "managed"
    std::string base_url;
    std::vector<std::string> api_key_env_vars;
    std::string docs_url;
    bool requires_subscription = false;
    bool supports_credential_pool = false;
};

const std::unordered_map<std::string, ProviderConfig>& provider_registry();

// Lookup with ``tolower`` normalization.
const ProviderConfig* find_provider(const std::string& provider_id);

// Returns true only if the user has explicitly configured this provider
// (active in auth.json, listed as model.provider in config.yaml, or
// provider-specific env var is set with a usable secret).  Implicit env
// vars like CLAUDE_CODE_OAUTH_TOKEN do not count.
//
// ``config_provider`` and ``env_lookup`` are injection points so tests
// can avoid touching the real config and environment.
using EnvLookupFn = std::function<std::string(const std::string&)>;
bool is_provider_explicitly_configured(const std::string& provider_id,
                                       const std::string& config_provider = {},
                                       const EnvLookupFn& env_lookup = {});

}  // namespace hermes::cli::auth_core
