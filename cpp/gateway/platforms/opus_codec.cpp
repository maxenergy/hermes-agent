// Phase 14 — Opus codec wrapper implementation.
#include "opus_codec.hpp"

#ifdef HERMES_GATEWAY_HAS_OPUS
#include <opus/opus.h>
#endif

namespace hermes::gateway::platforms {

OpusCodec::OpusCodec() {
#ifdef HERMES_GATEWAY_HAS_OPUS
    int err = 0;
    OpusEncoder* enc = opus_encoder_create(kSampleRate, kChannels,
                                           OPUS_APPLICATION_VOIP, &err);
    if (err == OPUS_OK && enc) {
        opus_encoder_ctl(enc, OPUS_SET_BITRATE(64000));
        encoder_ = enc;
    }
    OpusDecoder* dec = opus_decoder_create(kSampleRate, kChannels, &err);
    if (err == OPUS_OK && dec) {
        decoder_ = dec;
    }
#endif
}

OpusCodec::~OpusCodec() {
#ifdef HERMES_GATEWAY_HAS_OPUS
    if (encoder_) {
        opus_encoder_destroy(static_cast<OpusEncoder*>(encoder_));
        encoder_ = nullptr;
    }
    if (decoder_) {
        opus_decoder_destroy(static_cast<OpusDecoder*>(decoder_));
        decoder_ = nullptr;
    }
#endif
}

std::vector<uint8_t> OpusCodec::encode(const int16_t* pcm,
                                       std::size_t frame_samples) {
#ifdef HERMES_GATEWAY_HAS_OPUS
    if (!encoder_ || !pcm || frame_samples == 0) return {};
    std::vector<uint8_t> out(4000);  // Discord recommends <= 4000 bytes.
    int nbytes = opus_encode(static_cast<OpusEncoder*>(encoder_), pcm,
                             static_cast<int>(frame_samples), out.data(),
                             static_cast<opus_int32>(out.size()));
    if (nbytes < 0) return {};
    out.resize(static_cast<std::size_t>(nbytes));
    return out;
#else
    (void)pcm;
    (void)frame_samples;
    return {};
#endif
}

int OpusCodec::decode(const uint8_t* data, std::size_t len, int16_t* pcm_out,
                      std::size_t pcm_capacity) {
#ifdef HERMES_GATEWAY_HAS_OPUS
    if (!decoder_ || !pcm_out || pcm_capacity == 0) return 0;
    int max_per_channel = static_cast<int>(pcm_capacity / kChannels);
    int samples = opus_decode(static_cast<OpusDecoder*>(decoder_), data,
                              static_cast<opus_int32>(len), pcm_out,
                              max_per_channel, 0);
    if (samples < 0) return 0;
    return samples;
#else
    (void)data;
    (void)len;
    (void)pcm_out;
    (void)pcm_capacity;
    return 0;
#endif
}

bool OpusCodec::available() const {
#ifdef HERMES_GATEWAY_HAS_OPUS
    return encoder_ != nullptr && decoder_ != nullptr;
#else
    return false;
#endif
}

}  // namespace hermes::gateway::platforms
