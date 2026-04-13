// Phase 14 — Discord voice tests (libopus + SSRC mapping + voice callback).
#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <vector>

#include "../platforms/discord.hpp"
#include "../platforms/opus_codec.hpp"

using hermes::gateway::platforms::DiscordAdapter;
using hermes::gateway::platforms::OpusCodec;

namespace {

std::vector<int16_t> make_sine_frame(double freq_hz = 440.0,
                                     double amplitude = 0.5) {
    std::vector<int16_t> pcm(OpusCodec::kSamplesPerFrame * OpusCodec::kChannels);
    const double two_pi = 6.283185307179586;
    for (int i = 0; i < OpusCodec::kSamplesPerFrame; ++i) {
        double t = static_cast<double>(i) / OpusCodec::kSampleRate;
        double v = std::sin(two_pi * freq_hz * t) * amplitude;
        int16_t s = static_cast<int16_t>(v * 32767.0);
        pcm[i * 2 + 0] = s;
        pcm[i * 2 + 1] = s;
    }
    return pcm;
}

double rms(const int16_t* samples, std::size_t n) {
    if (n == 0) return 0.0;
    double acc = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        double v = static_cast<double>(samples[i]) / 32768.0;
        acc += v * v;
    }
    return std::sqrt(acc / static_cast<double>(n));
}

}  // namespace

TEST(OpusCodecBasic, AvailabilityMatchesCompileFlag) {
    OpusCodec codec;
#ifdef HERMES_GATEWAY_HAS_OPUS
    EXPECT_TRUE(codec.available());
#else
    EXPECT_FALSE(codec.available());
#endif
}

TEST(OpusCodecBasic, EncodeDecodeRoundTripPreservesEnergy) {
    OpusCodec codec;
    if (!codec.available()) {
        GTEST_SKIP() << "opus not compiled in";
    }
    auto pcm = make_sine_frame();
    double rms_in = rms(pcm.data(), pcm.size());

    auto pkt = codec.encode(pcm.data(), OpusCodec::kSamplesPerFrame);
    ASSERT_FALSE(pkt.empty());

    std::vector<int16_t> out(OpusCodec::kSamplesPerFrame * OpusCodec::kChannels);
    int samples = codec.decode(pkt.data(), pkt.size(), out.data(), out.size());
    EXPECT_EQ(samples, OpusCodec::kSamplesPerFrame);

    double rms_out = rms(out.data(), out.size());
    // Opus is lossy but should preserve signal energy within ~50%.
    EXPECT_GT(rms_out, rms_in * 0.5);
    EXPECT_LT(rms_out, rms_in * 1.5);
}

TEST(DiscordVoice, SsrcRegisterAndLookup) {
    DiscordAdapter::Config cfg;
    cfg.bot_token = "TKN";
    DiscordAdapter adapter(cfg);
    adapter.register_ssrc_user(12345u, "user-abc");
    auto got = adapter.ssrc_to_user(12345u);
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(*got, "user-abc");
}

TEST(DiscordVoice, UnregisteredSsrcReturnsNullopt) {
    DiscordAdapter::Config cfg;
    cfg.bot_token = "TKN";
    DiscordAdapter adapter(cfg);
    auto got = adapter.ssrc_to_user(99999u);
    EXPECT_FALSE(got.has_value());
}

TEST(DiscordVoice, SendVoicePcmWithoutConnectionFails) {
    DiscordAdapter::Config cfg;
    cfg.bot_token = "TKN";
    DiscordAdapter adapter(cfg);
    auto pcm = make_sine_frame();
    EXPECT_FALSE(adapter.send_voice_pcm(pcm.data(), OpusCodec::kSamplesPerFrame));
}

TEST(DiscordVoice, VoiceCallbackSetterGetter) {
    DiscordAdapter::Config cfg;
    cfg.bot_token = "TKN";
    DiscordAdapter adapter(cfg);
    EXPECT_FALSE(adapter.has_voice_callback());
    adapter.set_voice_callback(
        [](const DiscordAdapter::VoicePacket&) {});
    EXPECT_TRUE(adapter.has_voice_callback());
    adapter.set_voice_callback({});
    EXPECT_FALSE(adapter.has_voice_callback());
}

TEST(DiscordVoice, LeaveVoiceResetsState) {
    DiscordAdapter::Config cfg;
    cfg.bot_token = "TKN";
    DiscordAdapter adapter(cfg);
    EXPECT_FALSE(adapter.voice_connected());
    if (adapter.voice_codec().available()) {
        EXPECT_TRUE(adapter.join_voice("channel-1"));
        EXPECT_TRUE(adapter.voice_connected());
    }
    adapter.leave_voice();
    EXPECT_FALSE(adapter.voice_connected());
}

TEST(DiscordVoice, ProcessVoiceRtpInvokesCallback) {
    DiscordAdapter::Config cfg;
    cfg.bot_token = "TKN";
    DiscordAdapter adapter(cfg);
    if (!adapter.voice_codec().available()) {
        GTEST_SKIP() << "opus not compiled in";
    }
    auto pcm = make_sine_frame();
    auto opus_pkt = adapter.voice_codec().encode(pcm.data(),
                                                  OpusCodec::kSamplesPerFrame);
    ASSERT_FALSE(opus_pkt.empty());

    // Build a minimal RTP frame: 12-byte header + payload.
    std::vector<uint8_t> rtp(12 + opus_pkt.size(), 0);
    // sequence = 7
    rtp[2] = 0x00; rtp[3] = 0x07;
    // timestamp = 960
    rtp[4] = 0x00; rtp[5] = 0x00; rtp[6] = 0x03; rtp[7] = 0xC0;
    // ssrc = 0xDEADBEEF
    rtp[8] = 0xDE; rtp[9] = 0xAD; rtp[10] = 0xBE; rtp[11] = 0xEF;
    std::copy(opus_pkt.begin(), opus_pkt.end(), rtp.begin() + 12);

    bool called = false;
    DiscordAdapter::VoicePacket captured;
    adapter.set_voice_callback([&](const DiscordAdapter::VoicePacket& vp) {
        called = true;
        captured = vp;
    });
    EXPECT_TRUE(adapter.process_voice_rtp(rtp.data(), rtp.size()));
    EXPECT_TRUE(called);
    EXPECT_EQ(captured.ssrc, 0xDEADBEEFu);
    EXPECT_EQ(captured.sequence, 7u);
    EXPECT_EQ(captured.timestamp, 960u);
    EXPECT_FALSE(captured.pcm.empty());
}

TEST(DiscordVoice, ProcessVoiceRtpRejectsShortPacket) {
    DiscordAdapter::Config cfg;
    cfg.bot_token = "TKN";
    DiscordAdapter adapter(cfg);
    uint8_t tiny[4] = {0, 0, 0, 0};
    EXPECT_FALSE(adapter.process_voice_rtp(tiny, sizeof(tiny)));
}
