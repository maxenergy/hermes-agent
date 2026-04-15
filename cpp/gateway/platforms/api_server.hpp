// Phase 12 — OpenAI-compatible API Server platform adapter (depth port).
//
// Mirrors gateway/platforms/api_server.py.  We do not embed an HTTP server
// in the C++ build (that would require pulling in aiohttp-equivalent infra);
// instead we port the request/response data layer, validation logic, error
// envelopes, idempotency cache, response store (LRU + conversation map),
// CORS resolution, and chat-completion / responses-API payload builders so
// the same wire contract can be used from a future C++ HTTP front-end.
#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <list>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include <nlohmann/json.hpp>

#include <hermes/gateway/gateway_runner.hpp>

namespace hermes::gateway::platforms {

inline constexpr const char* kApiServerDefaultHost = "127.0.0.1";
inline constexpr int kApiServerDefaultPort = 8642;
inline constexpr std::size_t kApiServerMaxStoredResponses = 100;
inline constexpr std::size_t kApiServerMaxRequestBytes = 1'000'000;
inline constexpr std::size_t kApiServerMaxNameLength = 200;
inline constexpr std::size_t kApiServerMaxPromptLength = 5000;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

// OpenAI-style error envelope.
nlohmann::json api_openai_error(const std::string& message,
                                const std::string& err_type = "invalid_request_error",
                                const std::string& param = "",
                                const std::string& code = "");

// Derive a stable session ID from the system prompt + first user message.
// Hash with SHA256 and take the first 16 hex chars; prefix with "api-".
std::string api_derive_chat_session_id(const std::string& system_prompt,
                                       const std::string& first_user_message);

// Build a deterministic request fingerprint for idempotency caching.
std::string api_make_request_fingerprint(const nlohmann::json& body,
                                         const std::vector<std::string>& keys);

// Validate a job_id formatted as exactly 12 lowercase hex chars.
bool api_is_valid_job_id(const std::string& job_id);

// Whitelist of allowed cron-job update fields (matches Python adapter).
const std::vector<std::string>& api_update_allowed_fields();

// Resolve the advertised model name.  Empty input + empty profile → fallback.
std::string api_resolve_model_name(const std::string& explicit_name,
                                   const std::string& profile_name);

// Parse a CORS origin spec ("a.com,b.com,*") into a normalized vector.
std::vector<std::string> api_parse_cors_origins(const std::string& raw);

// Resolve CORS headers for an inbound Origin header.
// Returns std::nullopt when the origin is not allowed.
std::optional<std::unordered_map<std::string, std::string>>
api_cors_headers_for_origin(const std::vector<std::string>& configured,
                            const std::string& origin);

// Decide whether an origin is allowed (empty origin = non-browser, allowed).
bool api_origin_allowed(const std::vector<std::string>& configured,
                        const std::string& origin);

// Validate a session ID — reject control characters that could enable
// header-injection attacks.
bool api_is_valid_session_id(const std::string& session_id);

// Validate Content-Length for incoming POST/PUT/PATCH requests.
// Returns 0 when within limits, the rejection HTTP status otherwise.
int api_check_body_size(const std::string& content_length_header);

// Extract system prompt + last user message + history from the OpenAI
// chat-completion `messages` array.
struct ChatCompletionInput {
    std::string system_prompt;
    std::string user_message;
    std::vector<nlohmann::json> history;
    std::string error;
};
ChatCompletionInput api_parse_chat_completion_messages(
    const nlohmann::json& messages);

// Build the OpenAI chat.completion response object.
nlohmann::json api_build_chat_completion_response(
    const std::string& completion_id,
    const std::string& model,
    std::int64_t created,
    const std::string& final_response,
    std::int64_t input_tokens,
    std::int64_t output_tokens);

// Build the SSE finish chunk (final usage chunk).
nlohmann::json api_build_chat_completion_finish_chunk(
    const std::string& completion_id,
    const std::string& model,
    std::int64_t created,
    std::int64_t input_tokens,
    std::int64_t output_tokens);

// Build the OpenAI responses-API response object.
nlohmann::json api_build_response_object(
    const std::string& response_id,
    const std::string& model,
    std::int64_t created_at,
    const nlohmann::json& output_items,
    std::int64_t input_tokens,
    std::int64_t output_tokens);

// Validate Bearer token from an Authorization header.  Returns true when no
// API key is configured (all requests allowed) OR when the header presents a
// matching Bearer token.
bool api_check_bearer_auth(const std::string& configured_key,
                           const std::string& auth_header);

// ---------------------------------------------------------------------------
// ResponseStore — LRU cache of stored responses + conversation→response map.
// ---------------------------------------------------------------------------

class ResponseStore {
public:
    explicit ResponseStore(std::size_t max_size = kApiServerMaxStoredResponses);

    // Retrieve a stored response (refreshes LRU position).  Returns nullopt
    // when not found.
    std::optional<nlohmann::json> get(const std::string& response_id);

    // Insert or replace a stored response.  Evicts LRU entries beyond
    // max_size.
    void put(const std::string& response_id, nlohmann::json data);

    // Remove a stored response.  Returns true when the entry existed.
    bool remove(const std::string& response_id);

    // Conversation name → latest response_id mapping.
    std::optional<std::string> get_conversation(const std::string& name) const;
    void set_conversation(const std::string& name,
                          const std::string& response_id);

    std::size_t size() const;
    std::size_t max_size() const { return max_size_; }

private:
    mutable std::mutex mu_;
    std::size_t max_size_;
    // LRU tracking — list front=newest.
    std::list<std::string> lru_;
    std::unordered_map<std::string, std::pair<nlohmann::json,
                                              std::list<std::string>::iterator>>
        store_;
    std::unordered_map<std::string, std::string> conversations_;
};

// ---------------------------------------------------------------------------
// IdempotencyCache — TTL-bounded cache for Idempotency-Key replay.
// ---------------------------------------------------------------------------

class IdempotencyCache {
public:
    using Clock = std::chrono::steady_clock;

    explicit IdempotencyCache(std::size_t max_items = 1000,
                              std::chrono::seconds ttl = std::chrono::seconds(300));

    struct Entry {
        nlohmann::json response;
        std::string fingerprint;
        Clock::time_point ts;
    };

    // Look up an existing entry; returns nullopt when missing or expired or
    // when the fingerprint doesn't match.  Purges expired entries on call.
    std::optional<nlohmann::json> get(const std::string& key,
                                      const std::string& fingerprint);

    // Insert/replace.  Triggers purge + LRU eviction.
    void put(const std::string& key,
             const std::string& fingerprint,
             nlohmann::json response);

    std::size_t size() const;

private:
    void purge_locked();

    mutable std::mutex mu_;
    std::size_t max_items_;
    std::chrono::seconds ttl_;
    std::list<std::string> lru_;
    std::unordered_map<std::string,
                       std::pair<Entry, std::list<std::string>::iterator>>
        store_;
};

// ---------------------------------------------------------------------------
// Adapter
// ---------------------------------------------------------------------------

class ApiServerAdapter : public BasePlatformAdapter {
public:
    struct Config {
        std::string host = kApiServerDefaultHost;
        int port = kApiServerDefaultPort;
        std::string api_key;             // Bearer token (empty = no auth)
        std::string hmac_secret;         // optional HMAC verification
        std::vector<std::string> cors_origins;
        std::string model_name;          // empty → resolve from profile
        std::string profile_name;        // for model-name fallback
    };

    explicit ApiServerAdapter(Config cfg);

    // ----- BasePlatformAdapter ---------------------------------------------
    Platform platform() const override { return Platform::ApiServer; }
    bool connect() override;
    void disconnect() override;
    bool send(const std::string& chat_id, const std::string& content) override;
    void send_typing(const std::string& /*chat_id*/) override {}

    // ----- Pending response queue (legacy stub API) ------------------------
    std::string get_pending_response(const std::string& chat_id);

    // ----- HMAC ------------------------------------------------------------
    static bool verify_hmac_signature(const std::string& secret,
                                      const std::string& body,
                                      const std::string& signature_hex);

    // ----- Auth ------------------------------------------------------------
    bool check_auth(const std::string& auth_header) const;

    // ----- CORS ------------------------------------------------------------
    bool origin_allowed(const std::string& origin) const;
    std::optional<std::unordered_map<std::string, std::string>>
    cors_headers_for_origin(const std::string& origin) const;

    // ----- Model name ------------------------------------------------------
    std::string resolved_model_name() const;

    // ----- Stores ----------------------------------------------------------
    ResponseStore& response_store() { return response_store_; }
    IdempotencyCache& idempotency_cache() { return idempotency_cache_; }

    // ----- Accessors -------------------------------------------------------
    const Config& config() const { return cfg_; }
    bool connected() const { return connected_; }

private:
    Config cfg_;
    bool connected_ = false;
    ResponseStore response_store_;
    IdempotencyCache idempotency_cache_;
    std::mutex queue_mu_;
    std::unordered_map<std::string, std::string> pending_responses_;
};

}  // namespace hermes::gateway::platforms
