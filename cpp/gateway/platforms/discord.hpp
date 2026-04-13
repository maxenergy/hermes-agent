// Phase 12 — Discord platform adapter. Phase 14 — voice via libopus.
#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include <hermes/gateway/gateway_runner.hpp>
#include <hermes/llm/llm_client.hpp>

#include "opus_codec.hpp"

namespace hermes::gateway::platforms {

class DiscordAdapter : public BasePlatformAdapter {
public:
    struct Config {
        std::string bot_token;
        std::string application_id;
        bool manage_threads = true;
    };

    // A decoded 20ms frame from a remote speaker in a voice channel.
    struct VoicePacket {
        uint32_t ssrc = 0;           // sending user's SSRC
        uint16_t sequence = 0;
        uint32_t timestamp = 0;
        std::vector<int16_t> pcm;    // decoded stereo 48kHz interleaved
    };

    struct SsrcUserMapping {
        uint32_t ssrc;
        std::string user_id;
    };

    using VoiceCallback = std::function<void(const VoicePacket&)>;

    explicit DiscordAdapter(Config cfg);
    DiscordAdapter(Config cfg, hermes::llm::HttpTransport* transport);

    Platform platform() const override { return Platform::Discord; }
    bool connect() override;
    void disconnect() override;
    bool send(const std::string& chat_id, const std::string& content) override;
    void send_typing(const std::string& chat_id) override;

    // Format a Discord user mention from a numeric user ID.
    static std::string format_mention(const std::string& user_id);

    // Create a thread under a channel. Returns true on 2xx.
    bool create_thread(const std::string& channel_id,
                       const std::string& name,
                       int auto_archive_minutes = 1440);

    // Add an emoji reaction to a message.
    bool add_reaction(const std::string& channel_id,
                      const std::string& message_id,
                      const std::string& emoji);

    // ----- Voice API (Phase 14) -----

    // Connect to a voice channel. Returns true if we have a voice transport
    // available (currently requires a live voice gateway; stub returns false
    // until a real voice WebSocket is wired).
    bool join_voice(const std::string& channel_id);
    void leave_voice();
    bool voice_connected() const { return voice_connected_; }

    // Decoded PCM stream callback (each 20ms frame triggers the callback).
    void set_voice_callback(VoiceCallback cb);
    bool has_voice_callback() const;

    // Register/lookup SSRC↔user_id mapping (populated by SPEAKING events).
    void register_ssrc_user(uint32_t ssrc, const std::string& user_id);
    std::optional<std::string> ssrc_to_user(uint32_t ssrc) const;

    // Push PCM to the voice channel (e.g. TTS output). Opus-encoded and sent.
    // Returns false if no voice connection or opus unavailable.
    bool send_voice_pcm(const int16_t* pcm, std::size_t frames);

    // Internal: decode a raw RTP voice packet and invoke voice_cb_.
    // Returns true if the packet was successfully decoded and dispatched.
    // The payload is expected to already be decrypted opus.
    bool process_voice_rtp(const uint8_t* rtp, std::size_t len);

    // Stub hook for voice payload decryption. Real implementation needs
    // libsodium (xsalsa20_poly1305); returns false unless
    // HERMES_GATEWAY_HAS_SODIUM is defined.
    bool decrypt_voice_payload(const uint8_t* ciphertext,
                               std::size_t ct_len,
                               const uint8_t* nonce,
                               std::vector<uint8_t>& out_plain) const;

    OpusCodec& voice_codec() { return voice_codec_; }

    Config config() const { return cfg_; }
    bool connected() const { return connected_; }

private:
    hermes::llm::HttpTransport* get_transport();
    Config cfg_;
    hermes::llm::HttpTransport* transport_ = nullptr;
    bool connected_ = false;

    OpusCodec voice_codec_;
    bool voice_connected_ = false;
    std::string voice_channel_id_;
    mutable std::mutex ssrc_mu_;
    std::map<uint32_t, std::string> ssrc_map_;
    mutable std::mutex voice_cb_mu_;
    VoiceCallback voice_cb_;
};

}  // namespace hermes::gateway::platforms
