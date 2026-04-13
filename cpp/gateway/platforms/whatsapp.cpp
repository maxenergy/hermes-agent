// Phase 12 — WhatsApp platform adapter implementation.
#include "whatsapp.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>

#include <hermes/gateway/status.hpp>

namespace hermes::gateway::platforms {

namespace {
std::string whatsapp_lock_identity(const WhatsAppAdapter::Config& cfg) {
    // Prefer phone-number-id (graph API) since it's the credential that
    // Meta scopes per WABA.  Fall back to session_dir for whatsmeow.
    if (!cfg.phone.empty()) return "phone:" + cfg.phone;
    return "session:" + cfg.session_dir;
}

// Case-insensitive ends_with.
bool iends_with(const std::string& s, const std::string& suffix) {
    if (s.size() < suffix.size()) return false;
    return std::equal(
        suffix.rbegin(), suffix.rend(), s.rbegin(),
        [](char a, char b) {
            return std::tolower(static_cast<unsigned char>(a)) ==
                   std::tolower(static_cast<unsigned char>(b));
        });
}
}  // namespace

WhatsAppAdapter::WhatsAppAdapter(Config cfg) : cfg_(std::move(cfg)) {}

WhatsAppAdapter::WhatsAppAdapter(Config cfg, hermes::llm::HttpTransport* transport)
    : cfg_(std::move(cfg)), transport_(transport) {}

hermes::llm::HttpTransport* WhatsAppAdapter::get_transport() {
    if (transport_) return transport_;
    return hermes::llm::get_default_transport();
}

bool WhatsAppAdapter::connect() {
    if (cfg_.session_dir.empty() && cfg_.phone.empty()) return false;
    // Token-scoped lock: phone+session-dir is the credential.
    if (!hermes::gateway::acquire_scoped_lock(
            hermes::gateway::platform_to_string(platform()),
            whatsapp_lock_identity(cfg_), {})) {
        return false;
    }
    // WhatsApp uses whatsmeow bridge which requires WebSocket for receiving.
    // connect() notes this; send() can use HTTP API.
    return true;
}

void WhatsAppAdapter::disconnect() {
    if (!cfg_.session_dir.empty() || !cfg_.phone.empty()) {
        hermes::gateway::release_scoped_lock(
            hermes::gateway::platform_to_string(platform()),
            whatsapp_lock_identity(cfg_));
    }
}

bool WhatsAppAdapter::send(const std::string& chat_id,
                           const std::string& content) {
    // When we have an ephemeral timer remembered for this chat, honour it
    // automatically — callers don't need to track it.
    int timer = ephemeral_for(chat_id);
    if (timer > 0) {
        return send_with_ephemeral(chat_id, content, timer);
    }
    return send_with_ephemeral(chat_id, content, 0);
}

bool WhatsAppAdapter::send_with_ephemeral(const std::string& chat_id,
                                          const std::string& content,
                                          int timer_seconds) {
    auto* transport = get_transport();
    if (!transport) return false;

    std::string jid = resolve_jid(chat_id);

    // Bridge (whatsmeow/Baileys) path: POST {bridge}/send.
    if (!cfg_.bridge_url.empty()) {
        nlohmann::json payload = {
            {"to", jid},
            {"type", "text"},
            {"body", content},
        };
        if (timer_seconds > 0) {
            payload["ephemeral_expiration"] = timer_seconds;
        }
        try {
            auto resp = transport->post_json(
                cfg_.bridge_url + "/send",
                {{"Content-Type", "application/json"}}, payload.dump());
            return resp.status_code >= 200 && resp.status_code < 300;
        } catch (...) {
            return false;
        }
    }

    // WhatsApp Cloud API endpoint.
    nlohmann::json payload = {
        {"messaging_product", "whatsapp"},
        {"to", jid},
        {"type", "text"},
        {"text", {{"body", content}}}
    };
    // Graph API uses a distinct "context" / "ephemeral_expiration" top-level
    // field — set it when the chat has a disappearing-message timer.
    if (timer_seconds > 0) {
        payload["ephemeral_expiration"] = timer_seconds;
    }

    try {
        auto resp = transport->post_json(
            "https://graph.facebook.com/v17.0/" + cfg_.phone + "/messages",
            {{"Content-Type", "application/json"}},
            payload.dump());
        return resp.status_code >= 200 && resp.status_code < 300;
    } catch (...) {
        return false;
    }
}

void WhatsAppAdapter::send_typing(const std::string& /*chat_id*/) {}

std::optional<WhatsAppAdapter::PairingCode> WhatsAppAdapter::start_pairing(
    const std::string& phone) {
    if (phone.empty() || cfg_.bridge_url.empty()) return std::nullopt;
    auto* transport = get_transport();
    if (!transport) return std::nullopt;

    nlohmann::json payload = {{"phone", phone}};
    try {
        auto resp = transport->post_json(
            cfg_.bridge_url + "/pair",
            {{"Content-Type", "application/json"}}, payload.dump());
        if (resp.status_code < 200 || resp.status_code >= 300) {
            return std::nullopt;
        }
        auto j = nlohmann::json::parse(resp.body, nullptr, false);
        if (j.is_discarded()) return std::nullopt;
        PairingCode out;
        if (j.contains("code") && j["code"].is_string()) {
            out.code = j["code"].get<std::string>();
        } else {
            return std::nullopt;
        }
        out.phone = j.value("phone", phone);
        out.expires_in_seconds = j.value("expires_in", 0);
        return out;
    } catch (...) {
        return std::nullopt;
    }
}

int WhatsAppAdapter::parse_ephemeral_duration(const nlohmann::json& event) {
    if (!event.is_object()) return 0;
    // whatsmeow-style (camelCase).
    if (event.contains("ephemeralDuration") &&
        event["ephemeralDuration"].is_number_integer()) {
        return event["ephemeralDuration"].get<int>();
    }
    // Graph API / generic snake_case.
    if (event.contains("ephemeral_expiration") &&
        event["ephemeral_expiration"].is_number_integer()) {
        return event["ephemeral_expiration"].get<int>();
    }
    // Nested under "message" or "messageContextInfo".
    if (event.contains("message") && event["message"].is_object()) {
        return parse_ephemeral_duration(event["message"]);
    }
    if (event.contains("messageContextInfo") &&
        event["messageContextInfo"].is_object()) {
        auto& mci = event["messageContextInfo"];
        if (mci.contains("expiration") &&
            mci["expiration"].is_number_integer()) {
            return mci["expiration"].get<int>();
        }
    }
    return 0;
}

WhatsAppAdapter::ParticipantKind WhatsAppAdapter::classify_participant(
    const std::string& id) {
    if (id.empty()) return ParticipantKind::Unknown;
    if (iends_with(id, "@lid")) return ParticipantKind::Lid;
    if (iends_with(id, "@g.us")) return ParticipantKind::GroupJid;
    if (iends_with(id, "@broadcast")) return ParticipantKind::Broadcast;
    if (iends_with(id, "@s.whatsapp.net")) return ParticipantKind::LegacyJid;
    // Bare phone number (no @host) — treat as legacy JID candidate.
    if (!id.empty() && id.find('@') == std::string::npos) {
        return ParticipantKind::LegacyJid;
    }
    return ParticipantKind::Unknown;
}

std::optional<WhatsAppAdapter::GroupEvent>
WhatsAppAdapter::parse_group_event(const nlohmann::json& payload) {
    if (!payload.is_object()) return std::nullopt;

    GroupEvent ev;
    // Normalise the bridge's event envelope.  whatsmeow emits
    // {"type": "group", "action": "add", "group": "x@g.us",
    //  "participants": [...]} while Baileys uses "action" values like
    //  "promote" / "demote" / "subject" / "ephemeral".
    std::string action = payload.value("action", std::string{});
    std::string type = payload.value("type", std::string{});
    if (type != "group" && type != "groups-update" && action.empty()) {
        return std::nullopt;
    }

    ev.group_id = payload.value("group",
                                 payload.value("jid", std::string{}));
    if (ev.group_id.empty() && payload.contains("id")) {
        ev.group_id = payload["id"].get<std::string>();
    }
    // Must at least know which group.
    if (ev.group_id.empty()) return std::nullopt;

    if (action == "add") ev.type = GroupEventType::ParticipantsAdd;
    else if (action == "remove") ev.type = GroupEventType::ParticipantsRemove;
    else if (action == "promote") ev.type = GroupEventType::AdminPromote;
    else if (action == "demote") ev.type = GroupEventType::AdminDemote;
    else if (action == "subject") ev.type = GroupEventType::SubjectChange;
    else if (action == "ephemeral") ev.type = GroupEventType::EphemeralChange;
    else return std::nullopt;

    if (payload.contains("participants") &&
        payload["participants"].is_array()) {
        for (auto& p : payload["participants"]) {
            if (p.is_string()) {
                ev.participants.push_back(p.get<std::string>());
            } else if (p.is_object() && p.contains("id")) {
                ev.participants.push_back(p["id"].get<std::string>());
            }
        }
    }

    if (ev.type == GroupEventType::EphemeralChange) {
        ev.ephemeral_expiration_sec = payload.value(
            "ephemeral", payload.value("expiration", 0));
    }
    return ev;
}

std::string WhatsAppAdapter::resolve_jid(const std::string& phone) {
    // Already a fully-qualified identifier — pass through untouched.
    if (phone.find('@') != std::string::npos) return phone;
    return phone + "@s.whatsapp.net";
}

void WhatsAppAdapter::remember_ephemeral(const std::string& chat_id,
                                         int timer_seconds) {
    if (timer_seconds <= 0) {
        ephemeral_cache_.erase(chat_id);
    } else {
        ephemeral_cache_[chat_id] = timer_seconds;
    }
}

int WhatsAppAdapter::ephemeral_for(const std::string& chat_id) const {
    auto it = ephemeral_cache_.find(chat_id);
    if (it == ephemeral_cache_.end()) return 0;
    return it->second;
}

}  // namespace hermes::gateway::platforms
