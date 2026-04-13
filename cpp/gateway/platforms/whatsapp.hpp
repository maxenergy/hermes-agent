// Phase 12 — WhatsApp platform adapter.
#pragma once

#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include <nlohmann/json_fwd.hpp>

#include <hermes/gateway/gateway_runner.hpp>
#include <hermes/llm/llm_client.hpp>

namespace hermes::gateway::platforms {

class WhatsAppAdapter : public BasePlatformAdapter {
public:
    struct Config {
        std::string session_dir;
        std::string phone;
        // Local whatsmeow/Baileys bridge.  Empty => Graph Cloud API path.
        std::string bridge_url;
    };

    // 8-character pairing code returned by the bridge in response to a
    // phone-number registration.  `expires_in_seconds` is advisory; the
    // bridge is the source of truth.
    struct PairingCode {
        std::string code;           // e.g. "ABCD-EFGH"
        std::string phone;          // echoed back
        int expires_in_seconds = 0;
    };

    // Classification for a WhatsApp participant identifier.  LID ("linked
    // identifier") is WhatsApp's v2 group identity that is *not* a phone
    // number — it's opaque and privacy-preserving.  Legacy JIDs are of
    // the form "<phone>@s.whatsapp.net" and still appear in 1:1 chats and
    // in group rosters where the server has not migrated to LID.
    enum class ParticipantKind {
        Unknown,
        LegacyJid,   // <phone>@s.whatsapp.net
        Lid,         // <hash>@lid
        GroupJid,    // <id>@g.us
        Broadcast,   // <id>@broadcast
    };

    struct GroupParticipant {
        std::string id;              // canonical form (JID or LID)
        ParticipantKind kind = ParticipantKind::Unknown;
        bool is_admin = false;
        bool is_super_admin = false;
    };

    enum class GroupEventType {
        ParticipantsAdd,
        ParticipantsRemove,
        AdminPromote,
        AdminDemote,
        SubjectChange,
        EphemeralChange,
    };

    struct GroupEvent {
        GroupEventType type = GroupEventType::ParticipantsAdd;
        std::string group_id;        // <id>@g.us
        std::vector<std::string> participants;
        // For ephemeral changes: new expiration in seconds (0 == off).
        int ephemeral_expiration_sec = 0;
    };

    explicit WhatsAppAdapter(Config cfg);
    WhatsAppAdapter(Config cfg, hermes::llm::HttpTransport* transport);

    Platform platform() const override { return Platform::WhatsApp; }
    bool connect() override;
    void disconnect() override;
    bool send(const std::string& chat_id, const std::string& content) override;
    void send_typing(const std::string& chat_id) override;

    // Phase 12.5 — phone+code pairing.  Issues a pairing request to the
    // bridge's `/pair` endpoint (whatsmeow / Baileys).  Returns the
    // 8-char code the user types on their phone.  Returns std::nullopt on
    // transport failure or a non-2xx response.
    //
    // Wire: POST {bridge_url}/pair  { "phone": "+12125551234" }
    //      -> 200 { "code": "ABCDEFGH", "phone": "+12125551234",
    //               "expires_in": 60 }
    std::optional<PairingCode> start_pairing(const std::string& phone);

    // Phase 12.5 — disappearing messages.  When present on an inbound
    // event, callers (gateway main-loop) should record the timer and pass
    // it to `send_with_ephemeral()` when replying in that chat.
    //
    // `timer_seconds == 0` disables ephemeral mode.  WhatsApp currently
    // supports 0 / 86400 (24h) / 604800 (7d) / 7776000 (90d) but we
    // pass through whatever the server sent.
    bool send_with_ephemeral(const std::string& chat_id,
                             const std::string& content,
                             int timer_seconds);

    // Read ephemeral_expiration from an inbound event payload.
    // Returns 0 when no timer is set.  Accepts both whatsmeow-style
    // ("ephemeralDuration") and Graph-API-style ("ephemeral_expiration")
    // field names.
    static int parse_ephemeral_duration(const nlohmann::json& event);

    // Phase 12.5 — group v2.  Classify an identifier (JID vs LID vs group).
    static ParticipantKind classify_participant(const std::string& id);

    // Parse a group event payload emitted by the bridge.  Returns
    // std::nullopt when the JSON is not a recognisable group event.
    static std::optional<GroupEvent> parse_group_event(
        const nlohmann::json& payload);

    // Resolve JID/LID alias to canonical form.
    static std::string resolve_jid(const std::string& phone);

    // Record/lookup a per-chat ephemeral timer so the runner can echo it
    // on outbound replies.  Stateless in the adapter itself; this is a
    // small in-memory cache scoped to the adapter instance.
    void remember_ephemeral(const std::string& chat_id, int timer_seconds);
    int ephemeral_for(const std::string& chat_id) const;

    Config config() const { return cfg_; }

private:
    hermes::llm::HttpTransport* get_transport();
    Config cfg_;
    hermes::llm::HttpTransport* transport_ = nullptr;
    std::unordered_map<std::string, int> ephemeral_cache_;
};

}  // namespace hermes::gateway::platforms
