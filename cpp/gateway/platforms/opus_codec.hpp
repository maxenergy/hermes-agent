// Phase 14 — Opus codec wrapper for Discord voice.
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace hermes::gateway::platforms {

// Discord voice: 48kHz, 2ch, 20ms frames = 960 samples/ch.
// PCM is interleaved int16_t (little-endian).
class OpusCodec {
public:
    static constexpr int kSampleRate = 48000;
    static constexpr int kChannels = 2;
    static constexpr int kFrameMs = 20;
    static constexpr int kSamplesPerFrame = kSampleRate * kFrameMs / 1000;  // 960

    OpusCodec();   // encoder + decoder configured for Discord voice
    ~OpusCodec();

    OpusCodec(const OpusCodec&) = delete;
    OpusCodec& operator=(const OpusCodec&) = delete;

    // PCM [0..kSamplesPerFrame*kChannels) -> encoded opus packet.
    // Returns empty vector if opus is unavailable or on error.
    std::vector<uint8_t> encode(const int16_t* pcm,
                                std::size_t frame_samples = kSamplesPerFrame);

    // Opus packet -> PCM. Writes up to kSamplesPerFrame*kChannels samples.
    // Returns sample count (per channel) decoded, 0 on error.
    int decode(const uint8_t* data, std::size_t len, int16_t* pcm_out,
               std::size_t pcm_capacity);

    bool available() const;   // true when HERMES_GATEWAY_HAS_OPUS

private:
#ifdef HERMES_GATEWAY_HAS_OPUS
    void* encoder_ = nullptr;  // OpusEncoder*
    void* decoder_ = nullptr;  // OpusDecoder*
#endif
};

}  // namespace hermes::gateway::platforms
