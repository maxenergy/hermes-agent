// Phase 12 — Home Assistant platform adapter (depth port).
//
// Mirrors gateway/platforms/homeassistant.py.  Covers WebSocket auth/sub
// envelopes, state-change formatting per HA domain (climate / sensor /
// binary_sensor / light / switch / fan / alarm / generic), event filtering
// (watch_domains / watch_entities / ignore_entities / watch_all), per-entity
// cooldown bookkeeping, and persistent_notification REST send.
#pragma once

#include <chrono>
#include <cstddef>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>

#include <nlohmann/json.hpp>

#include <hermes/gateway/gateway_runner.hpp>
#include <hermes/llm/llm_client.hpp>

namespace hermes::gateway::platforms {

inline constexpr std::size_t kHaMaxMessageLength = 4096;

// Convert an HA URL (http/https) to its WebSocket (ws/wss) variant + path.
std::string ha_websocket_url(const std::string& base_url);

// Build the auth payload sent in response to the auth_required envelope.
nlohmann::json ha_build_auth_payload(const std::string& token);

// Build the subscribe_events payload (state_changed by default).
nlohmann::json ha_build_subscribe_events(int message_id,
                                         const std::string& event_type = "state_changed");

// Format a state_changed event per-domain.  Returns an empty optional when
// the message should be dropped (no new state, or unchanged value).
std::optional<std::string> ha_format_state_change(
    const std::string& entity_id, const nlohmann::json& old_state,
    const nlohmann::json& new_state);

// Extract the domain prefix from "<domain>.<object_id>".
std::string ha_extract_domain(const std::string& entity_id);

// Filter helper — returns true when the entity should be dropped.
bool ha_should_skip_entity(
    const std::string& entity_id,
    const std::unordered_set<std::string>& watch_domains,
    const std::unordered_set<std::string>& watch_entities,
    const std::unordered_set<std::string>& ignore_entities, bool watch_all);

class HomeAssistantAdapter : public BasePlatformAdapter {
public:
    struct Config {
        std::string hass_token;
        std::string hass_url = "http://localhost:8123";
        std::unordered_set<std::string> watch_domains;
        std::unordered_set<std::string> watch_entities;
        std::unordered_set<std::string> ignore_entities;
        bool watch_all = false;
        std::chrono::seconds cooldown{30};
    };

    explicit HomeAssistantAdapter(Config cfg);
    HomeAssistantAdapter(Config cfg, hermes::llm::HttpTransport* transport);

    // ----- BasePlatformAdapter --------------------------------------------
    Platform platform() const override { return Platform::HomeAssistant; }
    bool connect() override;
    void disconnect() override;
    bool send(const std::string& chat_id, const std::string& content) override;
    void send_typing(const std::string& /*chat_id*/) override {}

    // ----- Sequence ID ----------------------------------------------------
    int next_message_id();

    // ----- Cooldown -------------------------------------------------------
    bool record_event(const std::string& entity_id,
                      std::chrono::system_clock::time_point now);
    std::size_t cooldown_size() const;

    // ----- Outbound notification builders ---------------------------------
    nlohmann::json build_notification_payload(const std::string& message,
                                              const std::string& title = "Hermes Agent") const;

    // ----- Accessors -----------------------------------------------------
    Config config() const { return cfg_; }
    bool connected() const { return connected_; }

private:
    hermes::llm::HttpTransport* get_transport();

    Config cfg_;
    hermes::llm::HttpTransport* transport_ = nullptr;
    bool connected_ = false;
    int message_id_ = 0;

    mutable std::mutex cool_mu_;
    std::unordered_map<std::string, std::chrono::system_clock::time_point>
        last_event_time_;
};

}  // namespace hermes::gateway::platforms
