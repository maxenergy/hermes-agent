// Phase 12 — Matrix E2EE: wrappers over libolm for account, Olm (pairwise)
// session, and Megolm (group) session objects.  When libolm is not
// available at build time, all methods are no-ops that return empty
// strings / nullopt / false and `available()` returns false.
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

namespace hermes::gateway::platforms {

// Account: long-lived per-device identity (Ed25519 + Curve25519 keys).
class OlmAccount {
public:
    OlmAccount();
    ~OlmAccount();

    OlmAccount(const OlmAccount&) = delete;
    OlmAccount& operator=(const OlmAccount&) = delete;
    OlmAccount(OlmAccount&& other) noexcept;
    OlmAccount& operator=(OlmAccount&& other) noexcept;

    // Pickled state for persistence.
    std::string pickle(const std::string& passphrase) const;
    bool unpickle(const std::string& pickled, const std::string& passphrase);

    std::string identity_keys_json() const;   // {curve25519, ed25519}
    bool generate_one_time_keys(std::size_t count);
    std::string one_time_keys_json() const;
    void mark_keys_as_published();

    bool available() const;

    // Internal helper — exposes the underlying OlmAccount* for session
    // construction.  Returns nullptr when libolm is unavailable.
    void* raw() const;

private:
    void* account_ = nullptr;  // OlmAccount*
};

// Session: pairwise encrypted channel with another device.
class OlmSession {
public:
    OlmSession();
    ~OlmSession();

    OlmSession(const OlmSession&) = delete;
    OlmSession& operator=(const OlmSession&) = delete;
    OlmSession(OlmSession&& other) noexcept;
    OlmSession& operator=(OlmSession&& other) noexcept;

    // Create outbound session from our account + their identity key + a OTK.
    static std::optional<OlmSession> create_outbound(const OlmAccount& account,
                                                     const std::string& their_identity_key,
                                                     const std::string& their_one_time_key);

    // Parse an inbound pre-key message from their curve key.
    static std::optional<OlmSession> create_inbound(OlmAccount& account,
                                                    const std::string& their_identity_key,
                                                    const std::string& one_time_key_message);

    // Encrypt plaintext → ciphertext JSON {"type":0|1,"body":"..."}
    std::string encrypt(const std::string& plaintext);

    // Decrypt message body (type 0 = pre-key, 1 = normal).
    std::optional<std::string> decrypt(int type, const std::string& body);

    std::string pickle(const std::string& passphrase) const;
    bool unpickle(const std::string& pickled, const std::string& passphrase);

    bool matches_inbound_session(const std::string& msg) const;

    bool available() const;

private:
    void* session_ = nullptr;  // OlmSession*
};

// MegolmSession: room-level group encryption (many-to-many).
class MegolmOutboundSession {
public:
    MegolmOutboundSession();
    ~MegolmOutboundSession();

    MegolmOutboundSession(const MegolmOutboundSession&) = delete;
    MegolmOutboundSession& operator=(const MegolmOutboundSession&) = delete;
    MegolmOutboundSession(MegolmOutboundSession&& other) noexcept;
    MegolmOutboundSession& operator=(MegolmOutboundSession&& other) noexcept;

    std::string session_id() const;
    std::string session_key() const;   // share with new joiners
    std::uint32_t message_index() const;

    std::string encrypt(const std::string& plaintext);

    std::string pickle(const std::string& passphrase) const;
    bool unpickle(const std::string& pickled, const std::string& passphrase);
    bool available() const;

private:
    void* session_ = nullptr;  // OlmOutboundGroupSession*
};

class MegolmInboundSession {
public:
    MegolmInboundSession();
    ~MegolmInboundSession();

    MegolmInboundSession(const MegolmInboundSession&) = delete;
    MegolmInboundSession& operator=(const MegolmInboundSession&) = delete;
    MegolmInboundSession(MegolmInboundSession&& other) noexcept;
    MegolmInboundSession& operator=(MegolmInboundSession&& other) noexcept;

    bool init_from_session_key(const std::string& session_key);
    std::optional<std::string> decrypt(const std::string& ciphertext);
    std::string session_id() const;
    bool available() const;

private:
    void* session_ = nullptr;  // OlmInboundGroupSession*
};

}  // namespace hermes::gateway::platforms
