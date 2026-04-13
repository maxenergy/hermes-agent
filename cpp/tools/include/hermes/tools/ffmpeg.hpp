// Phase 8+: ffmpeg subprocess wrapper — media transcoding helpers.
//
// Wraps the `ffmpeg` and `ffprobe` binaries via std::system()-level
// subprocess execution for:
//
//   * Audio resample / format conversion (16 kHz mono PCM s16 <-> opus /
//     mp3 / wav / ogg) — used by the voice pipeline.
//   * Video transcode (mp4 <-> webm) — used by media tools.
//   * Thumbnail / frame extraction from video.
//   * Silence detection (VAD) via the `silencedetect` filter — used by
//     the streaming STT pipeline to carve segment boundaries.
//
// The binary path is auto-discovered via $PATH at first use.  If ffmpeg
// is not available, every call returns a FfmpegResult with
// ok == false and a clear install message in `error`.
#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace hermes::tools::ffmpeg {

// ---------------------------------------------------------------------------
// Result + configuration types.
// ---------------------------------------------------------------------------

struct FfmpegResult {
    bool ok = false;
    int exit_code = 0;
    std::string stdout_text;
    std::string stderr_text;
    std::string error;           // Human-readable summary when !ok.
    std::string command;         // Full command line (for debugging).
    std::string output_path;     // Populated for helpers that produce a file.
};

// Audio output containers / codecs the resample helper supports.
enum class AudioFormat { Wav, Mp3, Opus, Ogg, FlacRaw, PcmS16Le };

struct AudioResampleOptions {
    std::string input_path;
    std::string output_path;
    int sample_rate = 16000;
    int channels = 1;
    AudioFormat format = AudioFormat::Wav;
    int bitrate_kbps = 0;        // 0 = let ffmpeg pick.
    bool overwrite = true;
};

struct VideoTranscodeOptions {
    std::string input_path;
    std::string output_path;
    std::string video_codec;     // e.g. "libx264", "libvpx-vp9"; empty = copy.
    std::string audio_codec;     // e.g. "aac", "libopus"; empty = copy.
    int bitrate_kbps = 0;
    int width = 0;               // 0 = keep source dimensions.
    int height = 0;
    double fps = 0.0;
    bool overwrite = true;
};

struct ThumbnailOptions {
    std::string input_path;
    std::string output_path;
    double timestamp_seconds = 1.0;
    int width = 0;
    int height = 0;
    bool overwrite = true;
};

// One silence region reported by `silencedetect`.
struct SilenceRegion {
    double start_seconds = 0.0;
    double end_seconds = 0.0;
    double duration_seconds = 0.0;
};

struct SilenceDetectOptions {
    std::string input_path;
    // Noise threshold in dB below the 0 dBFS reference.  Default -30 dB.
    double noise_db = -30.0;
    // Minimum duration of silence (in seconds) to qualify as a split.
    double min_silence_seconds = 0.5;
};

// ---------------------------------------------------------------------------
// Binary discovery.
// ---------------------------------------------------------------------------

// Full path to the ffmpeg binary (or empty string if not on $PATH).
// Cached after first call; pass force_rescan=true in tests if you want a
// fresh lookup.
std::string locate_ffmpeg(bool force_rescan = false);
std::string locate_ffprobe(bool force_rescan = false);

// True iff both locate_ffmpeg() and locate_ffprobe() return non-empty.
bool is_available();

// Override the binary path for tests.  Pass empty string to reset to the
// auto-discovered value.
void set_ffmpeg_binary_for_testing(std::string path);
void set_ffprobe_binary_for_testing(std::string path);

// Hook to substitute the command executor for tests.  The hook receives
// argv and must populate stdout/stderr + exit_code on the result.  Reset
// with an empty std::function.
struct CommandInvocation {
    std::vector<std::string> argv;
};
struct CommandOutcome {
    int exit_code = -1;
    std::string stdout_text;
    std::string stderr_text;
};
using CommandRunner = std::function<CommandOutcome(const CommandInvocation&)>;
void set_command_runner_for_testing(CommandRunner runner);

// ---------------------------------------------------------------------------
// Helpers.
// ---------------------------------------------------------------------------

// Run an arbitrary ffmpeg invocation — argv already excludes the binary
// path, which this helper prepends.  Returns the raw outcome.
FfmpegResult run_ffmpeg(const std::vector<std::string>& argv,
                        std::chrono::seconds timeout = std::chrono::seconds{120});

// Audio resample / format conversion.
FfmpegResult resample_audio(const AudioResampleOptions& opts);

// Convert a WAV / mp3 / ... file to 16 kHz mono s16 PCM bytes (in
// memory).  On success `out_pcm` is populated with interleaved PCM data.
FfmpegResult decode_to_pcm16(const std::string& input_path,
                             int sample_rate,
                             int channels,
                             std::vector<int16_t>& out_pcm);

// Video transcode.
FfmpegResult transcode_video(const VideoTranscodeOptions& opts);

// Thumbnail extraction (single frame -> image file).
FfmpegResult extract_thumbnail(const ThumbnailOptions& opts);

// Silence detection — returns the parsed silence regions.
FfmpegResult detect_silence(const SilenceDetectOptions& opts,
                            std::vector<SilenceRegion>& out_regions);

// Probe a media file via ffprobe -> JSON.
FfmpegResult probe_media(const std::string& input_path, std::string& out_json);

}  // namespace hermes::tools::ffmpeg
