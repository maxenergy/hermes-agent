#include "hermes/tools/ffmpeg.hpp"

#include <gtest/gtest.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace hermes::tools::ffmpeg;

namespace {

class FfmpegTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Force a known "binary" so tests never depend on the real ffmpeg.
        set_ffmpeg_binary_for_testing("/usr/bin/ffmpeg-fake");
        set_ffprobe_binary_for_testing("/usr/bin/ffprobe-fake");
    }
    void TearDown() override {
        set_ffmpeg_binary_for_testing({});
        set_ffprobe_binary_for_testing({});
        set_command_runner_for_testing({});
    }
};

TEST_F(FfmpegTest, LocateHonoursOverride) {
    EXPECT_EQ(locate_ffmpeg(), "/usr/bin/ffmpeg-fake");
    EXPECT_EQ(locate_ffprobe(), "/usr/bin/ffprobe-fake");
    EXPECT_TRUE(is_available());
}

TEST_F(FfmpegTest, MissingBinaryReportsFriendlyError) {
    set_ffmpeg_binary_for_testing({});
    // Clear scanned cache by forcing rescan with a path that won't exist.
    // Instead, swap PATH so `which` returns empty.
    std::string prev_path = ::getenv("PATH") ? ::getenv("PATH") : "";
    ::setenv("PATH", "/nonexistent-hermes-path", 1);
    locate_ffmpeg(/*force_rescan=*/true);
    auto r = run_ffmpeg({"-version"});
    EXPECT_FALSE(r.ok);
    EXPECT_NE(r.error.find("ffmpeg binary not found"), std::string::npos);
    ::setenv("PATH", prev_path.c_str(), 1);
    locate_ffmpeg(/*force_rescan=*/true);
}

TEST_F(FfmpegTest, RunFfmpegPrependsBinary) {
    std::vector<std::string> captured_argv;
    set_command_runner_for_testing(
        [&](const CommandInvocation& inv) -> CommandOutcome {
            captured_argv = inv.argv;
            CommandOutcome o;
            o.exit_code = 0;
            o.stdout_text = "hello";
            return o;
        });

    auto r = run_ffmpeg({"-hide_banner", "-version"});
    EXPECT_TRUE(r.ok);
    EXPECT_EQ(r.exit_code, 0);
    EXPECT_EQ(r.stdout_text, "hello");
    ASSERT_GE(captured_argv.size(), 3u);
    EXPECT_EQ(captured_argv[0], "/usr/bin/ffmpeg-fake");
    EXPECT_EQ(captured_argv[1], "-hide_banner");
    EXPECT_EQ(captured_argv[2], "-version");
    EXPECT_NE(r.command.find("ffmpeg-fake"), std::string::npos);
}

TEST_F(FfmpegTest, ResampleBuildsPcmS16LeArgs) {
    std::vector<std::string> captured;
    set_command_runner_for_testing(
        [&](const CommandInvocation& inv) {
            captured = inv.argv;
            CommandOutcome o;
            o.exit_code = 0;
            return o;
        });

    AudioResampleOptions opts;
    opts.input_path = "/tmp/in.wav";
    opts.output_path = "/tmp/out.raw";
    opts.sample_rate = 16000;
    opts.channels = 1;
    opts.format = AudioFormat::PcmS16Le;
    auto r = resample_audio(opts);
    EXPECT_TRUE(r.ok);
    EXPECT_EQ(r.output_path, "/tmp/out.raw");

    auto contains = [&](const std::string& needle) {
        for (const auto& a : captured)
            if (a == needle) return true;
        return false;
    };
    EXPECT_TRUE(contains("-ar"));
    EXPECT_TRUE(contains("16000"));
    EXPECT_TRUE(contains("-ac"));
    EXPECT_TRUE(contains("1"));
    EXPECT_TRUE(contains("-f"));
    EXPECT_TRUE(contains("s16le"));
    EXPECT_TRUE(contains("/tmp/in.wav"));
    EXPECT_TRUE(contains("/tmp/out.raw"));
}

TEST_F(FfmpegTest, ResampleOpusBitrate) {
    std::vector<std::string> captured;
    set_command_runner_for_testing(
        [&](const CommandInvocation& inv) {
            captured = inv.argv;
            CommandOutcome o;
            o.exit_code = 0;
            return o;
        });

    AudioResampleOptions opts;
    opts.input_path = "in.wav";
    opts.output_path = "out.opus";
    opts.format = AudioFormat::Opus;
    opts.bitrate_kbps = 96;
    (void)resample_audio(opts);

    bool saw_codec_opus = false, saw_bitrate = false;
    for (std::size_t i = 0; i + 1 < captured.size(); ++i) {
        if (captured[i] == "-c:a" && captured[i + 1] == "libopus")
            saw_codec_opus = true;
        if (captured[i] == "-b:a" && captured[i + 1] == "96k")
            saw_bitrate = true;
    }
    EXPECT_TRUE(saw_codec_opus);
    EXPECT_TRUE(saw_bitrate);
}

TEST_F(FfmpegTest, DecodeToPcm16ReadsRawFile) {
    auto raw_bytes = std::vector<int16_t>{100, 200, 300, -400};
    set_command_runner_for_testing(
        [&](const CommandInvocation& inv) -> CommandOutcome {
            // Our helper passes the temp file as the last arg. Write our
            // fake PCM payload to that path so the caller can read it back.
            EXPECT_FALSE(inv.argv.empty());
            if (inv.argv.empty()) {
                CommandOutcome bad; bad.exit_code = 1; return bad;
            }
            const auto& out_path = inv.argv.back();
            std::ofstream ofs(out_path, std::ios::binary);
            ofs.write(reinterpret_cast<const char*>(raw_bytes.data()),
                      raw_bytes.size() * sizeof(int16_t));
            CommandOutcome o;
            o.exit_code = 0;
            return o;
        });

    std::vector<int16_t> pcm;
    auto r = decode_to_pcm16("/tmp/in.mp3", 16000, 1, pcm);
    ASSERT_TRUE(r.ok) << r.stderr_text;
    ASSERT_EQ(pcm.size(), raw_bytes.size());
    for (std::size_t i = 0; i < pcm.size(); ++i) {
        EXPECT_EQ(pcm[i], raw_bytes[i]);
    }
}

TEST_F(FfmpegTest, SilenceDetectParsesRegions) {
    set_command_runner_for_testing(
        [&](const CommandInvocation&) -> CommandOutcome {
            CommandOutcome o;
            o.exit_code = 0;
            // Typical ffmpeg silencedetect stderr output.
            o.stderr_text =
                "[silencedetect @ 0x55] silence_start: 1.25\n"
                "[silencedetect @ 0x55] silence_end: 2.75 | "
                "silence_duration: 1.5\n"
                "[silencedetect @ 0x55] silence_start: 5\n"
                "[silencedetect @ 0x55] silence_end: 5.75 | "
                "silence_duration: 0.75\n";
            return o;
        });

    SilenceDetectOptions opts;
    opts.input_path = "/tmp/audio.wav";
    opts.noise_db = -30.0;
    opts.min_silence_seconds = 0.5;

    std::vector<SilenceRegion> regions;
    auto r = detect_silence(opts, regions);
    EXPECT_TRUE(r.ok);
    ASSERT_EQ(regions.size(), 2u);
    EXPECT_DOUBLE_EQ(regions[0].start_seconds, 1.25);
    EXPECT_DOUBLE_EQ(regions[0].end_seconds, 2.75);
    EXPECT_DOUBLE_EQ(regions[0].duration_seconds, 1.5);
    EXPECT_DOUBLE_EQ(regions[1].start_seconds, 5.0);
    EXPECT_DOUBLE_EQ(regions[1].duration_seconds, 0.75);
}

TEST_F(FfmpegTest, TranscodeVideoUsesCodecArgs) {
    std::vector<std::string> captured;
    set_command_runner_for_testing(
        [&](const CommandInvocation& inv) {
            captured = inv.argv;
            CommandOutcome o;
            o.exit_code = 0;
            return o;
        });

    VideoTranscodeOptions opts;
    opts.input_path = "in.mp4";
    opts.output_path = "out.webm";
    opts.video_codec = "libvpx-vp9";
    opts.audio_codec = "libopus";
    opts.width = 640;
    opts.height = 360;
    auto r = transcode_video(opts);
    EXPECT_TRUE(r.ok);

    bool saw_vp9 = false, saw_opus = false, saw_size = false;
    for (std::size_t i = 0; i + 1 < captured.size(); ++i) {
        if (captured[i] == "-c:v" && captured[i + 1] == "libvpx-vp9") saw_vp9 = true;
        if (captured[i] == "-c:a" && captured[i + 1] == "libopus") saw_opus = true;
        if (captured[i] == "-s" && captured[i + 1] == "640x360") saw_size = true;
    }
    EXPECT_TRUE(saw_vp9);
    EXPECT_TRUE(saw_opus);
    EXPECT_TRUE(saw_size);
}

TEST_F(FfmpegTest, ThumbnailAppliesTimestamp) {
    std::vector<std::string> captured;
    set_command_runner_for_testing(
        [&](const CommandInvocation& inv) {
            captured = inv.argv;
            CommandOutcome o; o.exit_code = 0; return o;
        });
    ThumbnailOptions opts;
    opts.input_path = "video.mp4";
    opts.output_path = "thumb.png";
    opts.timestamp_seconds = 3.5;
    auto r = extract_thumbnail(opts);
    EXPECT_TRUE(r.ok);
    bool saw_ss = false, saw_frames = false;
    for (std::size_t i = 0; i + 1 < captured.size(); ++i) {
        if (captured[i] == "-ss") saw_ss = true;
        if (captured[i] == "-frames:v" && captured[i + 1] == "1") saw_frames = true;
    }
    EXPECT_TRUE(saw_ss);
    EXPECT_TRUE(saw_frames);
}

TEST_F(FfmpegTest, ProbeMediaReturnsStdoutJson) {
    set_command_runner_for_testing(
        [&](const CommandInvocation&) {
            CommandOutcome o;
            o.exit_code = 0;
            o.stdout_text = R"({"format":{"duration":"12.34"}})";
            return o;
        });
    std::string json;
    auto r = probe_media("/tmp/vid.mp4", json);
    EXPECT_TRUE(r.ok);
    EXPECT_NE(json.find("12.34"), std::string::npos);
}

TEST_F(FfmpegTest, NonZeroExitPropagates) {
    set_command_runner_for_testing(
        [&](const CommandInvocation&) {
            CommandOutcome o;
            o.exit_code = 2;
            o.stderr_text = "bad args";
            return o;
        });
    auto r = run_ffmpeg({"-version"});
    EXPECT_FALSE(r.ok);
    EXPECT_EQ(r.exit_code, 2);
    EXPECT_NE(r.error.find("status 2"), std::string::npos);
}

}  // namespace
