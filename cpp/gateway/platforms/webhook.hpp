// Phase 12 — Generic Webhook platform adapter (depth port).
//
// Mirrors gateway/platforms/webhook.py.  Covers HMAC signature verification
// (GitHub / GitLab / generic), event-type filtering, prompt template
// rendering with dot-notation, idempotency dedup, fixed-window per-route
// rate limiting, delivery-info bookkeeping with TTL pruning, route-table
// validation, and dynamic subscription merging.
#pragma once

#include <chrono>
#include <cstddef>
#include <deque>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include <nlohmann/json.hpp>

#include <hermes/gateway/gateway_runner.hpp>
#include <hermes/llm/llm_client.hpp>

namespace hermes::gateway::platforms {

inline constexpr const char* kWebhookInsecureNoAuth = "INSECURE_NO_AUTH";
inline constexpr int kWebhookDefaultPort = 8644;
inline constexpr std::size_t kWebhookDefaultMaxBodyBytes = 1'048'576;
inline constexpr int kWebhookDefaultRateLimit = 30;  // per minute
inline constexpr std::chrono::seconds kWebhookIdempotencyTtl{3600};

// ---------------------------------------------------------------------------
// Free helpers (depth port)
// ---------------------------------------------------------------------------

// HMAC-SHA256 of body using secret, returned as lowercase hex.
std::string webhook_compute_hmac_sha256(const std::string& secret,
                                        const std::string& body);

// Validate an inbound webhook signature using all known schemes:
//   * GitHub  (X-Hub-Signature-256: sha256=<hex>)
//   * GitLab  (X-Gitlab-Token: <plain secret>)
//   * Generic (X-Webhook-Signature: <hex>)
// Returns true when at least one scheme verifies.
bool webhook_validate_signature(
    const std::unordered_map<std::string, std::string>& headers,
    const std::string& body, const std::string& secret);

// Pick the event-type string from headers + payload, in priority order:
//   X-GitHub-Event > X-GitLab-Event > payload.event_type > "unknown".
std::string webhook_extract_event_type(
    const std::unordered_map<std::string, std::string>& headers,
    const nlohmann::json& payload);

// Render a prompt template with {dot.notation} resolved against payload.
// {__raw__} → 4000-char JSON dump of payload.  Missing keys preserve {key}.
std::string webhook_render_prompt(const std::string& tmpl,
                                  const nlohmann::json& payload,
                                  const std::string& event_type,
                                  const std::string& route_name);

// Render a delivery_extra map (string values get template substitution).
nlohmann::json webhook_render_delivery_extra(const nlohmann::json& extra,
                                             const nlohmann::json& payload);

// Build "webhook:<route>:<delivery_id>".
std::string webhook_build_session_chat_id(const std::string& route_name,
                                          const std::string& delivery_id);

// Pick a delivery_id from headers, falling back to a millisecond timestamp.
std::string webhook_extract_delivery_id(
    const std::unordered_map<std::string, std::string>& headers,
    std::chrono::system_clock::time_point now);

// ---------------------------------------------------------------------------
// Route + state types
// ---------------------------------------------------------------------------

struct WebhookRoute {
    std::string name;
    std::string secret;                  // empty → use global_secret
    std::vector<std::string> events;     // empty → accept all events
    std::string prompt;                  // template
    std::vector<std::string> skills;
    std::string deliver = "log";
    nlohmann::json deliver_extra = nlohmann::json::object();
};

struct WebhookValidateError {
    std::string route_name;
    std::string message;
};

// Validate that every route has a usable HMAC secret.
std::vector<WebhookValidateError> webhook_validate_routes(
    const std::unordered_map<std::string, WebhookRoute>& routes,
    const std::string& global_secret);

// ---------------------------------------------------------------------------
// Adapter
// ---------------------------------------------------------------------------

class WebhookAdapter : public BasePlatformAdapter {
public:
    struct Config {
        // Outbound (legacy): POST every send() to endpoint_url with optional
        // HMAC signature header.
        std::string signature_secret;
        std::string endpoint_url;

        // Inbound (depth port): receiver-side route table + per-route limits.
        std::string host = "0.0.0.0";
        int port = kWebhookDefaultPort;
        std::string global_secret;
        std::unordered_map<std::string, WebhookRoute> static_routes;
        int rate_limit_per_minute = kWebhookDefaultRateLimit;
        std::size_t max_body_bytes = kWebhookDefaultMaxBodyBytes;
    };

    explicit WebhookAdapter(Config cfg);
    WebhookAdapter(Config cfg, hermes::llm::HttpTransport* transport);

    // ----- BasePlatformAdapter --------------------------------------------
    Platform platform() const override { return Platform::Webhook; }
    bool connect() override;
    void disconnect() override;
    bool send(const std::string& chat_id, const std::string& content) override;
    void send_typing(const std::string& chat_id) override;

    // ----- HMAC helpers (legacy outbound API) ------------------------------
    static bool verify_hmac_signature(const std::string& secret,
                                      const std::string& body,
                                      const std::string& signature);
    static std::string compute_hmac(const std::string& secret,
                                    const std::string& body);

    // ----- Route management ------------------------------------------------
    const std::unordered_map<std::string, WebhookRoute>& routes() const {
        return effective_routes_;
    }
    bool has_route(const std::string& name) const;

    // Merge static + dynamic routes; static wins on collision.
    void set_dynamic_routes(std::unordered_map<std::string, WebhookRoute> dyn);

    // ----- Idempotency / rate limit / delivery info ------------------------
    bool seen_delivery(const std::string& delivery_id) const;
    void mark_delivery_seen(const std::string& delivery_id,
                            std::chrono::system_clock::time_point now);
    void prune_seen_deliveries(std::chrono::system_clock::time_point now);
    std::size_t seen_size() const;

    // True when within the per-minute rate limit; records on success.
    bool record_rate_limit(const std::string& route_name,
                           std::chrono::system_clock::time_point now);
    std::size_t rate_window_size(const std::string& route_name) const;

    void store_delivery_info(const std::string& session_chat_id,
                             nlohmann::json info,
                             std::chrono::system_clock::time_point now);
    std::optional<nlohmann::json> get_delivery_info(
        const std::string& session_chat_id) const;
    void prune_delivery_info(std::chrono::system_clock::time_point now);
    std::size_t delivery_info_size() const;

    // ----- Body size validation -------------------------------------------
    int check_body_size(std::size_t content_length) const;

    // ----- Accessors -------------------------------------------------------
    Config config() const { return cfg_; }
    bool connected() const { return connected_; }

private:
    hermes::llm::HttpTransport* get_transport();
    void rebuild_effective_routes();

    Config cfg_;
    hermes::llm::HttpTransport* transport_ = nullptr;
    bool connected_ = false;

    std::unordered_map<std::string, WebhookRoute> dynamic_routes_;
    std::unordered_map<std::string, WebhookRoute> effective_routes_;

    mutable std::mutex state_mu_;
    std::unordered_map<std::string, std::chrono::system_clock::time_point>
        seen_deliveries_;
    std::unordered_map<std::string,
                       std::deque<std::chrono::system_clock::time_point>>
        rate_windows_;
    std::unordered_map<std::string,
                       std::pair<nlohmann::json,
                                 std::chrono::system_clock::time_point>>
        delivery_info_;
};

}  // namespace hermes::gateway::platforms
