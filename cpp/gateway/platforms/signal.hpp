// Phase 12 — Signal platform adapter.
#pragma once

#include <optional>
#include <string>
#include <unordered_map>

#include <hermes/gateway/gateway_runner.hpp>
#include <hermes/llm/llm_client.hpp>

namespace hermes::gateway::platforms {

class SignalAdapter : public BasePlatformAdapter {
public:
    struct Config {
        std::string http_url;  // signal-cli REST API URL
        std::string account;   // phone number or UUID
    };

    // Signal safety-number verification state.  signal-cli returns three
    // possible trust levels per recipient: UNTRUSTED (first contact or
    // rotated key), VERIFIED (user marked the number as safe), and
    // TRUSTED_UNVERIFIED (we've messaged them before but not verified).
    enum class TrustLevel {
        Unknown,
        Untrusted,
        TrustedUnverified,
        Verified,
    };

    struct SafetyNumber {
        std::string recipient;
        std::string fingerprint;   // 60-digit or hex form returned by the API
        TrustLevel trust = TrustLevel::Unknown;
    };

    explicit SignalAdapter(Config cfg);
    SignalAdapter(Config cfg, hermes::llm::HttpTransport* transport);

    Platform platform() const override { return Platform::Signal; }
    bool connect() override;
    void disconnect() override;
    bool send(const std::string& chat_id, const std::string& content) override;
    void send_typing(const std::string& chat_id) override;

    // Phase 12.6 — disappearing messages.
    //
    // signal-cli exposes `setExpiration` which configures the per-recipient
    // (or per-group) expiration timer in seconds.  Passing 0 disables
    // disappearing messages.  Returns true on a 2xx response.
    bool set_expiration(const std::string& recipient, int expiration_seconds);

    // Send a message while honouring a remembered per-chat expiration
    // timer.  Also sets `expires_in_seconds` on the outbound payload so
    // the Signal server enforces the TTL independently of client state.
    bool send_with_expiration(const std::string& recipient,
                              const std::string& content,
                              int expiration_seconds);

    // Remembered expiration per-recipient (set by inbound event handler,
    // consumed by send()).  In-memory only.
    void remember_expiration(const std::string& recipient, int seconds);
    int expiration_for(const std::string& recipient) const;

    // Phase 12.6 — safety numbers.  Hits the signal-cli REST endpoint
    // /v1/identities/{account} and filters by `recipient`.  Returns the
    // parsed entry, or std::nullopt if the recipient has no known
    // identity or the call failed.
    std::optional<SafetyNumber> fetch_safety_number(
        const std::string& recipient);

    // Convenience: true when the identity is VERIFIED.  Returns false on
    // any other trust level, including Unknown.  This is what callers
    // should gate "safe to send" on.
    bool verify_safety_number(const std::string& recipient);

    // Trust a recipient's key.  When `verified == true` this upgrades the
    // trust level to VERIFIED; otherwise it only acknowledges
    // (TrustedUnverified).  POST /v1/identities/{account}/trust/{recipient}.
    bool trust_identity(const std::string& recipient, bool verified);

    // Phase 12.6 — group v2.  Normalise a chat identifier:
    // numbers => `+E.164`, groups prefixed with `@group.` or a bare
    // base64 group-id => `group.<id>`, UUIDs => lower-case UUID.
    static std::string normalize_identifier(const std::string& id);

    // Returns true when `id` names a Signal group (group.* form, or
    // explicit `@group` prefix used by the runner).
    static bool is_group_identifier(const std::string& id);

    Config config() const { return cfg_; }

private:
    hermes::llm::HttpTransport* get_transport();
    Config cfg_;
    hermes::llm::HttpTransport* transport_ = nullptr;
    std::unordered_map<std::string, int> expiration_cache_;
};

}  // namespace hermes::gateway::platforms
