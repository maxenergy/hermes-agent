// Depth port of gateway/platforms/api_server.py pure helpers.
//
// The full aiohttp-based OpenAI-compatible server is out of scope for
// this header; we concentrate on the testable building blocks that the
// Python module exposes alongside the adapter:
//
//   * OpenAI-style error envelope
//   * Idempotency cache (TTL + LRU) backed by an in-memory map
//   * Request fingerprinting (SHA-256 over a subset of body fields)
//   * Chat-session ID derivation (stable hash of system prompt +
//     first user message) used to reuse Hermes sessions across a
//     conversation's turns
//   * CORS origin parsing + lookup
//   * Model-name resolution (explicit → active profile → default)
//   * Output-item extraction (builds the Responses API items from a
//     tool-using agent trajectory)
//
// None of these require a running HTTP server, which keeps the port
// trivially unit-testable.
#pragma once

#include <chrono>
#include <cstddef>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <nlohmann/json.hpp>

namespace hermes::gateway::api_server_depth {

// --- OpenAI error envelope ----------------------------------------------

// Build ``{"error": {"message": ..., "type": ..., "param": ...,
// "code": ...}}``.  ``param`` and ``code`` default to ``null`` when
// empty.  Type defaults to ``invalid_request_error``.
nlohmann::json openai_error(std::string_view message,
                             std::string_view err_type = "invalid_request_error",
                             std::string_view param = {},
                             std::string_view code = {});

// --- Request fingerprinting --------------------------------------------

// Compute a SHA-256 hex digest over a subset of JSON body fields.  Keys
// that are absent contribute ``null`` (matching Python's
// ``body.get(k)``).  The subset is serialized in a deterministic order
// so two semantically-equal bodies hash to the same digest regardless
// of key ordering in the original JSON.
std::string make_request_fingerprint(const nlohmann::json& body,
                                       const std::vector<std::string>& keys);

// --- Session ID derivation ---------------------------------------------

// Derive a stable ``api-<16-hex>`` session ID from the conversation's
// system prompt (nullable) and first user message.  Mirrors
// ``_derive_chat_session_id`` verbatim so that a C++ client writing
// the same prompt/user text gets the same session key as Python.
std::string derive_chat_session_id(std::string_view system_prompt,
                                     std::string_view first_user_message);

// --- CORS origin parsing / lookup --------------------------------------

// Normalize a comma-separated string or a list of origins into a
// cleaned vector.  Accepts JSON arrays/strings.  Whitespace-only and
// empty entries are skipped.
std::vector<std::string> parse_cors_origins(const nlohmann::json& value);
std::vector<std::string> parse_cors_origins(std::string_view value);

// Return true when ``origin`` is allowed given the configured list.
// Empty ``origin`` (non-browser clients) is always allowed.  A wildcard
// ``*`` in the list allows any origin.
bool origin_allowed(const std::vector<std::string>& allowed,
                     std::string_view origin);

// Build the response headers for an allowed origin.  Returns an empty
// optional when the origin is not allowed.  The output always contains
// the ``Access-Control-Allow-*`` base headers plus either ``*`` or the
// exact origin echoed back with ``Vary: Origin``.
std::optional<std::map<std::string, std::string>>
cors_headers_for_origin(const std::vector<std::string>& allowed,
                          std::string_view origin);

// --- Model name resolution ---------------------------------------------

// Resolve the advertised model name.  Priority:
//   1. ``explicit`` if non-empty
//   2. ``profile`` if non-empty and not "default"/"custom"
//   3. fallback (``hermes-agent``)
std::string resolve_model_name(std::string_view explicit_name,
                                 std::string_view profile = {},
                                 std::string_view fallback = "hermes-agent");

// --- Idempotency cache --------------------------------------------------

// Thread-safe TTL + LRU cache for idempotent request replay.
//
// ``get_or_compute`` looks up ``key``; if present and the fingerprint
// matches, the cached JSON payload is returned without running the
// compute function.  Otherwise ``compute`` is invoked and its output
// stored (evicting the oldest entry when full).
class IdempotencyCache {
public:
    struct Clock {
        // Override for deterministic tests.
        std::function<std::chrono::steady_clock::time_point()> now;
    };

    IdempotencyCache(std::size_t max_items = 1000,
                      std::chrono::seconds ttl = std::chrono::seconds(300),
                      Clock clock = {});

    // Returns the cached JSON when the fingerprint matches, otherwise
    // invokes ``compute`` and caches its return value.
    nlohmann::json get_or_compute(
        const std::string& key, const std::string& fingerprint,
        const std::function<nlohmann::json()>& compute);

    // Direct accessors (mostly for tests).
    std::optional<nlohmann::json> peek(const std::string& key,
                                          const std::string& fingerprint);
    void put(const std::string& key, const std::string& fingerprint,
             nlohmann::json value);
    std::size_t size() const;
    std::size_t max_items() const { return max_; }
    std::chrono::seconds ttl() const { return ttl_; }

    // Purge expired entries.  Thread-safe.
    void purge();

private:
    struct Entry {
        std::string fingerprint;
        nlohmann::json value;
        std::chrono::steady_clock::time_point ts;
    };

    std::chrono::steady_clock::time_point clock_now() const;

    mutable std::mutex mu_;
    std::size_t max_;
    std::chrono::seconds ttl_;
    Clock clock_;
    // Insertion-ordered map of key -> entry.  ``access_order_`` tracks
    // eviction order; the newest entry goes to the back.
    std::unordered_map<std::string, Entry> store_;
    std::deque<std::string> access_order_;
};

// --- Output items extraction -------------------------------------------

// Build the ``output`` array for a Responses API result from the
// agent's message list.  Emits one ``function_call`` item per
// tool_call, one ``function_call_output`` item per tool message, and a
// final ``message`` item carrying the assistant's text reply.  The
// ``final`` text falls back to ``error`` when empty, then to a generic
// "no response" string.
nlohmann::json extract_output_items(const nlohmann::json& result);

// --- Body-limit classification -----------------------------------------

// Given the incoming ``Content-Length`` header value and a max body
// size in bytes, return:
//   * ``Ok`` when the length is absent or within the limit
//   * ``TooLarge`` when the length exceeds the limit
//   * ``Invalid`` when the header fails to parse as an integer
enum class BodyLimitStatus {
    Ok,
    TooLarge,
    Invalid,
};
BodyLimitStatus classify_body_length(std::string_view content_length,
                                       std::size_t max_bytes);

// --- Bearer token comparison -------------------------------------------

// Extract the token from an ``Authorization: Bearer <token>`` header
// and return true if it equals ``api_key`` via constant-time
// comparison.  Empty ``api_key`` disables auth and always returns true.
bool check_bearer_auth(std::string_view auth_header,
                        std::string_view api_key);

}  // namespace hermes::gateway::api_server_depth
