// Phase 12 — Matrix platform adapter.
#pragma once

#include <map>
#include <optional>
#include <string>
#include <utility>

#include <hermes/gateway/gateway_runner.hpp>
#include <hermes/llm/llm_client.hpp>

#include "olm_session.hpp"

namespace hermes::gateway::platforms {

class MatrixAdapter : public BasePlatformAdapter {
public:
    struct Config {
        std::string homeserver;
        std::string username;
        std::string password;
        std::string access_token;
        std::string device_id;
        // Passphrase used to encrypt the pickled olm account on disk.
        // Defaults to a constant; callers should override in production.
        std::string pickle_passphrase = "hermes-default";
    };

    explicit MatrixAdapter(Config cfg);
    MatrixAdapter(Config cfg, hermes::llm::HttpTransport* transport);

    Platform platform() const override { return Platform::Matrix; }
    bool connect() override;
    void disconnect() override;
    bool send(const std::string& chat_id, const std::string& content) override;
    void send_typing(const std::string& chat_id) override;

    Config config() const { return cfg_; }

    // E2EE: set up the olm account (create or load pickle) and upload device
    // keys via /keys/upload.  Returns true on success; when libolm is not
    // available at build time, returns true after a no-op (E2EE disabled).
    bool setup_e2ee();

    // Room-message encryption helpers.  When libolm is unavailable OR when no
    // Megolm session exists for the room, both methods pass the plaintext
    // through unchanged so that the clear-text transport path still works.
    bool encrypt_room_message(const std::string& room_id,
                              const std::string& plaintext,
                              std::string& out_ciphertext);
    std::optional<std::string> decrypt_room_message(const std::string& room_id,
                                                    const std::string& ciphertext);

    bool e2ee_enabled() const { return olm_account_.available(); }

private:
    hermes::llm::HttpTransport* get_transport();
    std::string olm_pickle_path() const;

    Config cfg_;
    hermes::llm::HttpTransport* transport_ = nullptr;
    std::string access_token_;

    // E2EE state.
    OlmAccount olm_account_;
    std::map<std::pair<std::string, std::string>, OlmSession> olm_sessions_;
    std::map<std::string, MegolmOutboundSession> megolm_out_;                                // room_id → session
    std::map<std::string, std::map<std::string, MegolmInboundSession>> megolm_in_;           // room_id → session_id → session
};

}  // namespace hermes::gateway::platforms
