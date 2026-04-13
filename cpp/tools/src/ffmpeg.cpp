// Phase 8+: ffmpeg subprocess wrapper implementation.
//
// No direct libavcodec / libavformat linkage — we shell out to the
// `ffmpeg` / `ffprobe` binaries.  This keeps the build dependency
// surface small (same strategy as voice_mode_tool + whisper CLI), and
// lets the operator swap in whichever ffmpeg flavour they have
// installed (system, conda, static, etc.).
#include "hermes/tools/ffmpeg.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>
#include <regex>
#include <sstream>

#include <sys/wait.h>
#include <unistd.h>

namespace hermes::tools::ffmpeg {

namespace {

// --------------------------------------------------------------------------
// Process-global state.  All access goes through `state_mutex()`.
// --------------------------------------------------------------------------

std::mutex& state_mutex() {
    static std::mutex m;
    return m;
}

struct State {
    std::string ffmpeg_binary;
    std::string ffprobe_binary;
    std::string ffmpeg_override;
    std::string ffprobe_override;
    CommandRunner runner_override;
    bool ffmpeg_scanned = false;
    bool ffprobe_scanned = false;
};

State& state() {
    static State s;
    return s;
}

// --------------------------------------------------------------------------
// PATH lookup.
// --------------------------------------------------------------------------

std::string which(const std::string& bin) {
    const char* path = std::getenv("PATH");
    if (!path) return {};
    std::string acc;
    auto try_path = [&](const std::string& dir) -> std::string {
        if (dir.empty()) return {};
        std::filesystem::path candidate =
            std::filesystem::path(dir) / bin;
        std::error_code ec;
        if (std::filesystem::is_regular_file(candidate, ec) ||
            std::filesystem::is_symlink(candidate, ec)) {
            if (::access(candidate.c_str(), X_OK) == 0) {
                return candidate.string();
            }
        }
        return {};
    };
    for (const char* p = path; ; ++p) {
        if (*p == ':' || *p == '\0') {
            auto found = try_path(acc);
            if (!found.empty()) return found;
            acc.clear();
            if (*p == '\0') break;
        } else {
            acc.push_back(*p);
        }
    }
    return {};
}

// --------------------------------------------------------------------------
// Default executor — fork + execvp with pipes for stdout/stderr.
// --------------------------------------------------------------------------

CommandOutcome default_runner(const CommandInvocation& inv) {
    CommandOutcome out;
    if (inv.argv.empty()) {
        out.exit_code = -1;
        out.stderr_text = "empty argv";
        return out;
    }

    int stdout_pipe[2];
    int stderr_pipe[2];
    if (pipe(stdout_pipe) != 0 || pipe(stderr_pipe) != 0) {
        out.exit_code = -1;
        out.stderr_text = std::string("pipe() failed: ") + std::strerror(errno);
        return out;
    }

    pid_t pid = fork();
    if (pid < 0) {
        out.exit_code = -1;
        out.stderr_text = std::string("fork() failed: ") + std::strerror(errno);
        return out;
    }
    if (pid == 0) {
        // Child
        dup2(stdout_pipe[1], STDOUT_FILENO);
        dup2(stderr_pipe[1], STDERR_FILENO);
        close(stdout_pipe[0]);
        close(stdout_pipe[1]);
        close(stderr_pipe[0]);
        close(stderr_pipe[1]);

        std::vector<char*> cargv;
        cargv.reserve(inv.argv.size() + 1);
        // NB: we need mutable C strings for execvp.
        std::vector<std::string> owned = inv.argv;
        for (auto& s : owned) cargv.push_back(&s[0]);
        cargv.push_back(nullptr);
        execvp(cargv[0], cargv.data());
        _exit(127);
    }

    // Parent
    close(stdout_pipe[1]);
    close(stderr_pipe[1]);

    auto drain = [](int fd, std::string& dest) {
        std::array<char, 4096> buf;
        while (true) {
            ssize_t n = ::read(fd, buf.data(), buf.size());
            if (n > 0) {
                dest.append(buf.data(), static_cast<size_t>(n));
            } else if (n == 0) {
                break;
            } else {
                if (errno == EINTR) continue;
                break;
            }
        }
    };
    drain(stdout_pipe[0], out.stdout_text);
    drain(stderr_pipe[0], out.stderr_text);
    close(stdout_pipe[0]);
    close(stderr_pipe[0]);

    int status = 0;
    while (true) {
        pid_t r = waitpid(pid, &status, 0);
        if (r == -1 && errno == EINTR) continue;
        break;
    }
    if (WIFEXITED(status)) {
        out.exit_code = WEXITSTATUS(status);
    } else if (WIFSIGNALED(status)) {
        out.exit_code = 128 + WTERMSIG(status);
    } else {
        out.exit_code = -1;
    }
    return out;
}

CommandOutcome run_invocation(const CommandInvocation& inv) {
    CommandRunner runner;
    {
        std::lock_guard<std::mutex> lk(state_mutex());
        runner = state().runner_override;
    }
    if (runner) return runner(inv);
    return default_runner(inv);
}

std::string join_cmdline(const std::vector<std::string>& argv) {
    std::string out;
    for (std::size_t i = 0; i < argv.size(); ++i) {
        if (i) out.push_back(' ');
        out.append(argv[i]);
    }
    return out;
}

// Maps AudioFormat -> ffmpeg codec/format arguments.
void apply_audio_format(std::vector<std::string>& argv,
                        AudioFormat fmt,
                        int bitrate_kbps) {
    switch (fmt) {
        case AudioFormat::Wav:
            argv.insert(argv.end(), {"-c:a", "pcm_s16le", "-f", "wav"});
            break;
        case AudioFormat::Mp3:
            argv.insert(argv.end(), {"-c:a", "libmp3lame", "-f", "mp3"});
            if (bitrate_kbps > 0) {
                argv.push_back("-b:a");
                argv.push_back(std::to_string(bitrate_kbps) + "k");
            }
            break;
        case AudioFormat::Opus:
            argv.insert(argv.end(), {"-c:a", "libopus", "-f", "opus"});
            if (bitrate_kbps > 0) {
                argv.push_back("-b:a");
                argv.push_back(std::to_string(bitrate_kbps) + "k");
            }
            break;
        case AudioFormat::Ogg:
            argv.insert(argv.end(), {"-c:a", "libvorbis", "-f", "ogg"});
            if (bitrate_kbps > 0) {
                argv.push_back("-b:a");
                argv.push_back(std::to_string(bitrate_kbps) + "k");
            }
            break;
        case AudioFormat::FlacRaw:
            argv.insert(argv.end(), {"-c:a", "flac", "-f", "flac"});
            break;
        case AudioFormat::PcmS16Le:
            argv.insert(argv.end(), {"-f", "s16le", "-c:a", "pcm_s16le"});
            break;
    }
}

FfmpegResult missing_binary_error() {
    FfmpegResult r;
    r.ok = false;
    r.error = "ffmpeg binary not found on PATH — install ffmpeg "
              "(apt install ffmpeg / brew install ffmpeg / "
              "choco install ffmpeg) or set HERMES_FFMPEG_BIN.";
    r.exit_code = -1;
    return r;
}

}  // namespace

// --------------------------------------------------------------------------
// Binary discovery.
// --------------------------------------------------------------------------

std::string locate_ffmpeg(bool force_rescan) {
    std::lock_guard<std::mutex> lk(state_mutex());
    auto& s = state();
    if (!s.ffmpeg_override.empty()) return s.ffmpeg_override;
    if (s.ffmpeg_scanned && !force_rescan) return s.ffmpeg_binary;

    if (const char* env = std::getenv("HERMES_FFMPEG_BIN"); env && *env) {
        s.ffmpeg_binary = env;
    } else {
        s.ffmpeg_binary = which("ffmpeg");
    }
    s.ffmpeg_scanned = true;
    return s.ffmpeg_binary;
}

std::string locate_ffprobe(bool force_rescan) {
    std::lock_guard<std::mutex> lk(state_mutex());
    auto& s = state();
    if (!s.ffprobe_override.empty()) return s.ffprobe_override;
    if (s.ffprobe_scanned && !force_rescan) return s.ffprobe_binary;
    if (const char* env = std::getenv("HERMES_FFPROBE_BIN"); env && *env) {
        s.ffprobe_binary = env;
    } else {
        s.ffprobe_binary = which("ffprobe");
    }
    s.ffprobe_scanned = true;
    return s.ffprobe_binary;
}

bool is_available() {
    return !locate_ffmpeg().empty();
}

void set_ffmpeg_binary_for_testing(std::string path) {
    std::lock_guard<std::mutex> lk(state_mutex());
    state().ffmpeg_override = std::move(path);
}

void set_ffprobe_binary_for_testing(std::string path) {
    std::lock_guard<std::mutex> lk(state_mutex());
    state().ffprobe_override = std::move(path);
}

void set_command_runner_for_testing(CommandRunner runner) {
    std::lock_guard<std::mutex> lk(state_mutex());
    state().runner_override = std::move(runner);
}

// --------------------------------------------------------------------------
// Helpers.
// --------------------------------------------------------------------------

FfmpegResult run_ffmpeg(const std::vector<std::string>& argv,
                        std::chrono::seconds /*timeout*/) {
    std::string bin = locate_ffmpeg();
    if (bin.empty()) return missing_binary_error();

    CommandInvocation inv;
    inv.argv.reserve(argv.size() + 1);
    inv.argv.push_back(bin);
    for (const auto& a : argv) inv.argv.push_back(a);

    auto outcome = run_invocation(inv);
    FfmpegResult r;
    r.command = join_cmdline(inv.argv);
    r.exit_code = outcome.exit_code;
    r.stdout_text = std::move(outcome.stdout_text);
    r.stderr_text = std::move(outcome.stderr_text);
    r.ok = (outcome.exit_code == 0);
    if (!r.ok) {
        r.error = "ffmpeg exited with status " +
                  std::to_string(outcome.exit_code);
    }
    return r;
}

FfmpegResult resample_audio(const AudioResampleOptions& opts) {
    if (opts.input_path.empty() || opts.output_path.empty()) {
        FfmpegResult r;
        r.error = "input_path/output_path required";
        return r;
    }
    std::vector<std::string> argv;
    if (opts.overwrite) argv.push_back("-y");
    else argv.push_back("-n");
    argv.insert(argv.end(), {"-hide_banner", "-loglevel", "error"});
    argv.insert(argv.end(), {"-i", opts.input_path});
    argv.insert(argv.end(),
                {"-ar", std::to_string(opts.sample_rate),
                 "-ac", std::to_string(opts.channels)});
    apply_audio_format(argv, opts.format, opts.bitrate_kbps);
    argv.push_back(opts.output_path);
    auto r = run_ffmpeg(argv);
    if (r.ok) r.output_path = opts.output_path;
    return r;
}

FfmpegResult decode_to_pcm16(const std::string& input_path,
                             int sample_rate,
                             int channels,
                             std::vector<int16_t>& out_pcm) {
    out_pcm.clear();
    std::string bin = locate_ffmpeg();
    if (bin.empty()) return missing_binary_error();

    auto tmpdir = std::filesystem::temp_directory_path();
    auto raw_path = tmpdir / ("hermes_ffmpeg_pcm_" +
                              std::to_string(::getpid()) + "_" +
                              std::to_string(reinterpret_cast<uintptr_t>(&out_pcm)) +
                              ".raw");

    AudioResampleOptions opts;
    opts.input_path = input_path;
    opts.output_path = raw_path.string();
    opts.sample_rate = sample_rate;
    opts.channels = channels;
    opts.format = AudioFormat::PcmS16Le;
    auto r = resample_audio(opts);
    if (!r.ok) {
        std::error_code ec;
        std::filesystem::remove(raw_path, ec);
        return r;
    }

    std::ifstream ifs(raw_path, std::ios::binary);
    if (!ifs) {
        r.ok = false;
        r.error = "ffmpeg succeeded but PCM temp file could not be read";
        std::error_code ec;
        std::filesystem::remove(raw_path, ec);
        return r;
    }
    ifs.seekg(0, std::ios::end);
    std::streamsize sz = ifs.tellg();
    ifs.seekg(0, std::ios::beg);
    if (sz > 0) {
        out_pcm.resize(static_cast<size_t>(sz / 2));
        ifs.read(reinterpret_cast<char*>(out_pcm.data()), (sz / 2) * 2);
    }
    std::error_code ec;
    std::filesystem::remove(raw_path, ec);
    r.output_path = input_path;  // semantic: input was decoded
    return r;
}

FfmpegResult transcode_video(const VideoTranscodeOptions& opts) {
    if (opts.input_path.empty() || opts.output_path.empty()) {
        FfmpegResult r;
        r.error = "input_path/output_path required";
        return r;
    }
    std::vector<std::string> argv;
    argv.push_back(opts.overwrite ? "-y" : "-n");
    argv.insert(argv.end(), {"-hide_banner", "-loglevel", "error"});
    argv.insert(argv.end(), {"-i", opts.input_path});
    if (!opts.video_codec.empty()) {
        argv.insert(argv.end(), {"-c:v", opts.video_codec});
    } else {
        argv.insert(argv.end(), {"-c:v", "copy"});
    }
    if (!opts.audio_codec.empty()) {
        argv.insert(argv.end(), {"-c:a", opts.audio_codec});
    } else {
        argv.insert(argv.end(), {"-c:a", "copy"});
    }
    if (opts.bitrate_kbps > 0) {
        argv.push_back("-b:v");
        argv.push_back(std::to_string(opts.bitrate_kbps) + "k");
    }
    if (opts.width > 0 && opts.height > 0) {
        argv.push_back("-s");
        argv.push_back(std::to_string(opts.width) + "x" +
                       std::to_string(opts.height));
    }
    if (opts.fps > 0.0) {
        std::ostringstream oss;
        oss << opts.fps;
        argv.push_back("-r");
        argv.push_back(oss.str());
    }
    argv.push_back(opts.output_path);
    auto r = run_ffmpeg(argv);
    if (r.ok) r.output_path = opts.output_path;
    return r;
}

FfmpegResult extract_thumbnail(const ThumbnailOptions& opts) {
    if (opts.input_path.empty() || opts.output_path.empty()) {
        FfmpegResult r;
        r.error = "input_path/output_path required";
        return r;
    }
    std::vector<std::string> argv;
    argv.push_back(opts.overwrite ? "-y" : "-n");
    argv.insert(argv.end(), {"-hide_banner", "-loglevel", "error"});
    {
        std::ostringstream oss;
        oss << opts.timestamp_seconds;
        argv.insert(argv.end(), {"-ss", oss.str()});
    }
    argv.insert(argv.end(), {"-i", opts.input_path});
    argv.insert(argv.end(), {"-frames:v", "1"});
    if (opts.width > 0 && opts.height > 0) {
        argv.push_back("-s");
        argv.push_back(std::to_string(opts.width) + "x" +
                       std::to_string(opts.height));
    }
    argv.push_back(opts.output_path);
    auto r = run_ffmpeg(argv);
    if (r.ok) r.output_path = opts.output_path;
    return r;
}

FfmpegResult detect_silence(const SilenceDetectOptions& opts,
                            std::vector<SilenceRegion>& out_regions) {
    out_regions.clear();
    if (opts.input_path.empty()) {
        FfmpegResult r;
        r.error = "input_path required";
        return r;
    }
    std::ostringstream filter;
    filter << "silencedetect=noise=" << opts.noise_db << "dB"
           << ":d=" << opts.min_silence_seconds;
    std::vector<std::string> argv = {
        "-hide_banner",
        "-nostats",
        "-i", opts.input_path,
        "-af", filter.str(),
        "-f", "null", "-"
    };
    auto r = run_ffmpeg(argv);
    // silencedetect writes to stderr even on success.
    // Parse lines: "silence_start: N.NN" and "silence_end: N.NN | silence_duration: M.MM".
    std::istringstream iss(r.stderr_text);
    std::string line;
    std::optional<double> pending_start;
    std::regex re_start(R"(silence_start:\s*(-?\d+(?:\.\d+)?))");
    std::regex re_end(
        R"(silence_end:\s*(-?\d+(?:\.\d+)?)\s*\|\s*silence_duration:\s*(-?\d+(?:\.\d+)?))");
    while (std::getline(iss, line)) {
        std::smatch m;
        if (std::regex_search(line, m, re_start)) {
            pending_start = std::stod(m[1].str());
        } else if (std::regex_search(line, m, re_end)) {
            SilenceRegion reg;
            reg.end_seconds = std::stod(m[1].str());
            reg.duration_seconds = std::stod(m[2].str());
            reg.start_seconds = pending_start.value_or(
                reg.end_seconds - reg.duration_seconds);
            out_regions.push_back(reg);
            pending_start.reset();
        }
    }
    return r;
}

FfmpegResult probe_media(const std::string& input_path, std::string& out_json) {
    out_json.clear();
    std::string bin = locate_ffprobe();
    if (bin.empty()) {
        FfmpegResult r;
        r.error = "ffprobe binary not found on PATH — install ffmpeg.";
        r.exit_code = -1;
        return r;
    }
    CommandInvocation inv;
    inv.argv = {
        bin,
        "-v", "error",
        "-print_format", "json",
        "-show_format",
        "-show_streams",
        input_path,
    };
    auto outcome = run_invocation(inv);
    FfmpegResult r;
    r.command = join_cmdline(inv.argv);
    r.exit_code = outcome.exit_code;
    r.stdout_text = std::move(outcome.stdout_text);
    r.stderr_text = std::move(outcome.stderr_text);
    r.ok = (outcome.exit_code == 0);
    if (r.ok) {
        out_json = r.stdout_text;
    } else {
        r.error = "ffprobe exited with status " +
                  std::to_string(outcome.exit_code);
    }
    return r;
}

}  // namespace hermes::tools::ffmpeg
