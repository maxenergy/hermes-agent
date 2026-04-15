// Phase 12 — Home Assistant adapter implementation (depth port).
#include "home_assistant.hpp"

#include <algorithm>
#include <cctype>
#include <sstream>

namespace hermes::gateway::platforms {

namespace {

bool starts_with_ci(const std::string& s, const std::string& prefix) {
    if (s.size() < prefix.size()) return false;
    for (std::size_t i = 0; i < prefix.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(s[i])) !=
            std::tolower(static_cast<unsigned char>(prefix[i]))) {
            return false;
        }
    }
    return true;
}

std::string trim_trailing_slash(std::string s) {
    while (!s.empty() && s.back() == '/') s.pop_back();
    return s;
}

std::string state_field(const nlohmann::json& state, const char* def = "unknown") {
    if (state.is_object() && state.contains("state") && state["state"].is_string()) {
        return state["state"].get<std::string>();
    }
    return def;
}

std::string attr_str(const nlohmann::json& state, const std::string& key,
                     const std::string& def) {
    if (!state.is_object()) return def;
    auto it = state.find("attributes");
    if (it == state.end() || !it->is_object()) return def;
    auto av = it->find(key);
    if (av == it->end()) return def;
    if (av->is_string()) return av->get<std::string>();
    if (av->is_number()) return av->dump();
    return def;
}

}  // namespace

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

std::string ha_websocket_url(const std::string& base_url) {
    std::string s = trim_trailing_slash(base_url);
    if (starts_with_ci(s, "https://")) return "wss://" + s.substr(8) + "/api/websocket";
    if (starts_with_ci(s, "http://")) return "ws://" + s.substr(7) + "/api/websocket";
    return "ws://" + s + "/api/websocket";
}

nlohmann::json ha_build_auth_payload(const std::string& token) {
    return nlohmann::json{{"type", "auth"}, {"access_token", token}};
}

nlohmann::json ha_build_subscribe_events(int message_id,
                                         const std::string& event_type) {
    return nlohmann::json{
        {"id", message_id},
        {"type", "subscribe_events"},
        {"event_type", event_type},
    };
}

std::string ha_extract_domain(const std::string& entity_id) {
    auto dot = entity_id.find('.');
    return dot == std::string::npos ? "" : entity_id.substr(0, dot);
}

bool ha_should_skip_entity(
    const std::string& entity_id,
    const std::unordered_set<std::string>& watch_domains,
    const std::unordered_set<std::string>& watch_entities,
    const std::unordered_set<std::string>& ignore_entities, bool watch_all) {
    if (entity_id.empty()) return true;
    if (ignore_entities.count(entity_id)) return true;
    std::string domain = ha_extract_domain(entity_id);
    if (!watch_domains.empty() || !watch_entities.empty()) {
        bool dom_match = !watch_domains.empty() && watch_domains.count(domain) > 0;
        bool ent_match = !watch_entities.empty() && watch_entities.count(entity_id) > 0;
        if (!dom_match && !ent_match) return true;
        return false;
    }
    return !watch_all;
}

std::optional<std::string> ha_format_state_change(
    const std::string& entity_id, const nlohmann::json& old_state,
    const nlohmann::json& new_state) {
    if (!new_state.is_object()) return std::nullopt;
    std::string old_val = state_field(old_state);
    std::string new_val = state_field(new_state);
    if (old_val == new_val) return std::nullopt;

    std::string friendly = attr_str(new_state, "friendly_name", entity_id);
    std::string domain = ha_extract_domain(entity_id);

    std::ostringstream oss;
    if (domain == "climate") {
        std::string temp = attr_str(new_state, "current_temperature", "?");
        std::string target = attr_str(new_state, "temperature", "?");
        oss << "[Home Assistant] " << friendly << ": HVAC mode changed from '"
            << old_val << "' to '" << new_val << "' (current: " << temp
            << ", target: " << target << ")";
    } else if (domain == "sensor") {
        std::string unit = attr_str(new_state, "unit_of_measurement", "");
        oss << "[Home Assistant] " << friendly << ": changed from " << old_val
            << unit << " to " << new_val << unit;
    } else if (domain == "binary_sensor") {
        oss << "[Home Assistant] " << friendly << ": "
            << (new_val == "on" ? "triggered" : "cleared") << " (was "
            << (old_val == "on" ? "triggered" : "cleared") << ")";
    } else if (domain == "light" || domain == "switch" || domain == "fan") {
        oss << "[Home Assistant] " << friendly << ": turned "
            << (new_val == "on" ? "on" : "off");
    } else if (domain == "alarm_control_panel") {
        oss << "[Home Assistant] " << friendly << ": alarm state changed from '"
            << old_val << "' to '" << new_val << "'";
    } else {
        oss << "[Home Assistant] " << friendly << " (" << entity_id
            << "): changed from '" << old_val << "' to '" << new_val << "'";
    }
    return oss.str();
}

// ---------------------------------------------------------------------------
// Adapter
// ---------------------------------------------------------------------------

HomeAssistantAdapter::HomeAssistantAdapter(Config cfg) : cfg_(std::move(cfg)) {
    cfg_.hass_url = trim_trailing_slash(cfg_.hass_url);
}

HomeAssistantAdapter::HomeAssistantAdapter(Config cfg,
                                           hermes::llm::HttpTransport* transport)
    : HomeAssistantAdapter(std::move(cfg)) {
    transport_ = transport;
}

hermes::llm::HttpTransport* HomeAssistantAdapter::get_transport() {
    if (transport_) return transport_;
    return hermes::llm::get_default_transport();
}

bool HomeAssistantAdapter::connect() {
    if (cfg_.hass_token.empty()) return false;
    auto* t = get_transport();
    if (!t) return false;
    try {
        auto resp = t->get(cfg_.hass_url + "/api/",
                           {{"Authorization", "Bearer " + cfg_.hass_token}});
        if (resp.status_code != 200) return false;
        connected_ = true;
        return true;
    } catch (...) {
        return false;
    }
}

void HomeAssistantAdapter::disconnect() {
    connected_ = false;
}

int HomeAssistantAdapter::next_message_id() { return ++message_id_; }

bool HomeAssistantAdapter::record_event(
    const std::string& entity_id,
    std::chrono::system_clock::time_point now) {
    std::lock_guard<std::mutex> lock(cool_mu_);
    auto it = last_event_time_.find(entity_id);
    if (it != last_event_time_.end() && now - it->second < cfg_.cooldown) {
        return false;
    }
    last_event_time_[entity_id] = now;
    return true;
}

std::size_t HomeAssistantAdapter::cooldown_size() const {
    std::lock_guard<std::mutex> lock(cool_mu_);
    return last_event_time_.size();
}

nlohmann::json HomeAssistantAdapter::build_notification_payload(
    const std::string& message, const std::string& title) const {
    std::string truncated = message.size() <= kHaMaxMessageLength
                                ? message
                                : message.substr(0, kHaMaxMessageLength);
    return nlohmann::json{{"title", title}, {"message", truncated}};
}

bool HomeAssistantAdapter::send(const std::string& /*chat_id*/,
                                const std::string& content) {
    auto* t = get_transport();
    if (!t) return false;
    try {
        auto resp = t->post_json(
            cfg_.hass_url + "/api/services/persistent_notification/create",
            {{"Authorization", "Bearer " + cfg_.hass_token},
             {"Content-Type", "application/json"}},
            build_notification_payload(content).dump());
        return resp.status_code >= 200 && resp.status_code < 300;
    } catch (...) {
        return false;
    }
}

}  // namespace hermes::gateway::platforms
