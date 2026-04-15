// Credential-pool depth helpers — pure-logic ports of the Python
// functions in agent/credential_pool.py that normalize error context,
// compute exhaustion cooldowns, parse provider reset timestamps, and
// derive custom-endpoint pool keys.
//
// These helpers do NOT perform I/O: config.yaml and auth.json access
// stays in the Python / state layers.  The depth module focuses on the
// exact arithmetic + string transforms the Python code does so the
// behaviour can be exercised from C++ tests.

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace hermes::agent::creds::depth {

// Strategy set mirrored from Python's SUPPORTED_POOL_STRATEGIES.  Used
// by get_pool_strategy() to reject unknown values.
bool is_supported_strategy(std::string_view name);

// Python helper _exhausted_ttl(error_code).
//
// Returns the cooldown (seconds) before retrying a credential whose
// request exhausted with the given HTTP status.  429 rate-limits and
// the default both map to 1 hour; a provider-supplied reset_at always
// wins when present (parse_absolute_timestamp + normalize).
std::int64_t exhausted_ttl(int error_code);

// Python helper _parse_absolute_timestamp(value).
//
// Accepts epoch seconds, epoch milliseconds (detected as "> 1e12"),
// and ISO-8601 strings.  Returns seconds since epoch.  Returns
// std::nullopt when the value is empty / non-positive / unparseable.
std::optional<double> parse_absolute_timestamp_numeric(double value);
std::optional<double> parse_absolute_timestamp_string(std::string_view raw);

// Python helper _extract_retry_delay_seconds(message).  Recognises the
// two surface forms that Nous / Codex / OpenRouter all emit:
//
//   * quotaResetDelay":"1500ms" / quotaResetDelay: 1500 ms
//   * retry after 60 seconds / retry 30s
//
// Returns std::nullopt when no delay is found in the message.
std::optional<double> extract_retry_delay_seconds(std::string_view message);

// Normalized form of an error context ({"reason","message","reset_at"}).
// Mirrors Python's _normalize_error_context.  Any field not present in
// the input is omitted from the output; reset_at is resolved via the
// (reset_at | resets_at | retry_until) fallback, or by applying
// extract_retry_delay_seconds(message) on top of `now_sec`.
struct NormalizedError {
    std::optional<std::string> reason;
    std::optional<std::string> message;
    std::optional<double> reset_at;  // absolute epoch seconds
};

struct RawErrorContext {
    std::string reason;
    std::string message;
    // All reset-at fallbacks the Python code accepts.  Empty string
    // means "not set".  Numeric values go through the numeric path.
    std::string reset_at_str;
    std::string resets_at_str;
    std::string retry_until_str;
    std::optional<double> reset_at_num;
};

NormalizedError normalize_error_context(const RawErrorContext& raw,
                                        double now_sec);

// Python helper _normalize_custom_pool_name — lowercase, trim, replace
// spaces with hyphens.
std::string normalize_custom_pool_name(std::string_view name);

// Python helper get_custom_provider_pool_key(base_url).  Given a list
// of (normalized_name, base_url) pairs from config.yaml's
// custom_providers, returns "custom:<name>" for the first match, or
// std::nullopt when none matches.  Both sides are normalized via
// `trim() + rtrim("/")` before comparison.
struct CustomProviderEntry {
    std::string normalized_name;
    std::string base_url;
};
std::optional<std::string> custom_provider_pool_key(
    std::string_view base_url,
    const std::vector<CustomProviderEntry>& providers);

// Python helper list_custom_pool_providers() — filter + sort over the
// keys of the credential-pool map.  Only returns keys that (a) start
// with "custom:" and (b) point to a non-empty list.
std::vector<std::string> list_custom_pool_providers(
    const std::vector<std::pair<std::string, std::size_t>>& pool_entries);

// Python helper _is_manual_source.  Accepts "manual" or "manual:<tag>"
// after trimming + lowercasing.
bool is_manual_source(std::string_view source);

// Python helper _next_priority — highest priority + 1, default 0.
int next_priority(const std::vector<int>& priorities);

// Python helper label_from_token(token, fallback).  When the JWT
// encodes an `email` / `preferred_username` / `upn` claim, return
// that; otherwise return fallback.  The JWT parse accepts claim dicts
// directly (tests decouple from base64 decoding).
std::string label_from_claims(
    const std::unordered_map<std::string, std::string>& claims,
    const std::string& fallback);

// Python helper _exhausted_until.  Given a credential's last status,
// status timestamp, error code, and optional provider reset_at, return
// the earliest time at which the credential becomes eligible again.
// Returns std::nullopt when the entry is not exhausted.
struct EntrySnapshot {
    std::string last_status;
    std::optional<double> last_status_at;
    std::optional<int> last_error_code;
    std::optional<double> last_error_reset_at;
};
std::optional<double> exhausted_until(const EntrySnapshot& entry);

// Pool-key prefix for custom endpoints — exposed for tests that want
// to share the string constant with the Python side.
constexpr std::string_view kCustomPoolPrefix = "custom:";

}  // namespace hermes::agent::creds::depth
