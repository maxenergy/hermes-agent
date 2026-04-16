// voice_mode tool — session state machine + shell-driven capture.

#include "hermes/tools/voice_mode_tool.hpp"

#include "hermes/tools/ffmpeg.hpp"
#include "hermes/tools/registry.hpp"
#include "hermes/tools/transcription_tool.hpp"

#include "hermes/environments/local.hpp"

#include <atomic>
#include <cctype>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#if !defined(_WIN32)
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace hermes::tools {

// ---------------------------------------------------------------------------
// Default recorder — forks `arecord`/`parecord`/`rec` and sends SIGTERM
// on stop().  Writes 16 kHz / mono / signed 16-bit PCM WAV to a file
// under $TMPDIR/hermes_voice/.
// ---------------------------------------------------------------------------

namespace {

std::string format_timestamp() {
    auto now = std::chrono::system_clock::now();
    auto t = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y%m%d_%H%M%S", &tm);
    return buf;
}

std::filesystem::path ensure_recording_dir() {
    auto dir = std::filesystem::temp_directory_path() / "hermes_voice";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    return dir;
}

// Look up a binary in $PATH.
std::string which(const std::string& bin) {
    hermes::environments::LocalEnvironment env;
    hermes::environments::ExecuteOptions opts;
    opts.timeout = std::chrono::seconds(3);
    auto r = env.execute("command -v " + bin + " 2>/dev/null", opts);
    if (r.exit_code != 0) return {};
    std::string out = r.stdout_text;
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r'))
        out.pop_back();
    return out;
}

enum class RecorderFlavour { None, Arecord, Parecord, SoxRec };

struct DiscoveredRecorder {
    RecorderFlavour flavour = RecorderFlavour::None;
    std::string path;
    std::string name;
};

DiscoveredRecorder discover_recorder() {
    if (auto p = which("arecord"); !p.empty()) {
        return {RecorderFlavour::Arecord, p, "arecord"};
    }
    if (auto p = which("parecord"); !p.empty()) {
        return {RecorderFlavour::Parecord, p, "parecord"};
    }
    if (auto p = which("rec"); !p.empty()) {
        return {RecorderFlavour::SoxRec, p, "sox-rec"};
    }
    return {};
}

class SubprocessRecorder : public AudioRecorder {
public:
    SubprocessRecorder() = default;
    ~SubprocessRecorder() override {
        if (is_recording()) cancel();
    }

    std::string start(std::string& error) override {
#if defined(_WIN32)
        (void)error;
        error = "voice recording not supported on Windows yet";
        return {};
#else
        if (recording_.load()) {
            error = "already recording";
            return {};
        }
        auto bin = discover_recorder();
        if (bin.flavour == RecorderFlavour::None) {
            error =
                "no audio capture binary found on PATH — install "
                "`arecord` (alsa-utils), `parecord` (pulseaudio-utils), "
                "or `rec` (sox)";
            return {};
        }
        backend_name_ = bin.name;

        auto dir = ensure_recording_dir();
        auto path = dir / ("recording_" + format_timestamp() + ".wav");
        path_ = path.string();

        std::vector<std::string> argv;
        argv.push_back(bin.path);
        switch (bin.flavour) {
            case RecorderFlavour::Arecord:
                // -q quiet, -f S16_LE 16-bit LE, -c1 mono, -r16000, -t wav
                argv.insert(argv.end(),
                            {"-q", "-f", "S16_LE", "-c", "1", "-r", "16000",
                             "-t", "wav", path_});
                break;
            case RecorderFlavour::Parecord:
                argv.insert(argv.end(),
                            {"--channels=1", "--rate=16000",
                             "--format=s16le", "--file-format=wav", path_});
                break;
            case RecorderFlavour::SoxRec:
                argv.insert(argv.end(),
                            {"-q", "-c", "1", "-r", "16000", "-b", "16", "-e",
                             "signed-integer", path_});
                break;
            case RecorderFlavour::None:
                break;
        }

        pid_t pid = fork();
        if (pid < 0) {
            error = "fork() failed";
            return {};
        }
        if (pid == 0) {
            // child
            std::vector<char*> cargv;
            cargv.reserve(argv.size() + 1);
            for (auto& a : argv) cargv.push_back(&a[0]);
            cargv.push_back(nullptr);
            // detach from terminal to avoid SIGINT race
            setpgid(0, 0);
            execv(cargv[0], cargv.data());
            _exit(127);
        }
        pid_ = pid;
        start_time_ = std::chrono::steady_clock::now();
        recording_.store(true);
        return path_;
#endif
    }

    std::string stop() override {
#if defined(_WIN32)
        return {};
#else
        if (!recording_.load()) return {};
        kill(pid_, SIGTERM);
        int status = 0;
        // Wait up to 3 s, then SIGKILL.
        auto deadline =
            std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while (std::chrono::steady_clock::now() < deadline) {
            pid_t r = waitpid(pid_, &status, WNOHANG);
            if (r == pid_) break;
            if (r < 0) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        if (waitpid(pid_, &status, WNOHANG) == 0) {
            kill(pid_, SIGKILL);
            waitpid(pid_, &status, 0);
        }
        recording_.store(false);
        pid_ = -1;

        std::error_code ec;
        if (path_.empty() || !std::filesystem::exists(path_, ec) ||
            std::filesystem::file_size(path_, ec) == 0) {
            return {};
        }
        return path_;
#endif
    }

    void cancel() override {
        auto p = stop();
        if (!p.empty()) {
            std::error_code ec;
            std::filesystem::remove(p, ec);
        }
        path_.clear();
    }

    bool is_recording() const override { return recording_.load(); }

    std::chrono::milliseconds elapsed() const override {
        if (!recording_.load()) return std::chrono::milliseconds{0};
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start_time_);
    }

    std::string backend_name() const override {
        return backend_name_.empty() ? "subprocess" : backend_name_;
    }

private:
    std::atomic<bool> recording_{false};
#if defined(_WIN32)
    int pid_ = -1;
#else
    pid_t pid_ = -1;
#endif
    std::string path_;
    std::string backend_name_;
    std::chrono::steady_clock::time_point start_time_;
};

// ---------------------------------------------------------------------------
// Factory registry.
// ---------------------------------------------------------------------------

std::mutex& factory_mutex() {
    static std::mutex m;
    return m;
}

RecorderFactory& factory_slot() {
    static RecorderFactory f;
    return f;
}

std::unique_ptr<AudioRecorder> make_recorder() {
    RecorderFactory cb;
    {
        std::lock_guard<std::mutex> lk(factory_mutex());
        cb = factory_slot();
    }
    if (cb) return cb();
    return std::make_unique<SubprocessRecorder>();
}

}  // namespace

void set_recorder_factory_for_testing(RecorderFactory factory) {
    std::lock_guard<std::mutex> lk(factory_mutex());
    factory_slot() = std::move(factory);
}

// ---------------------------------------------------------------------------
// VoiceSession
// ---------------------------------------------------------------------------

VoiceSession& VoiceSession::instance() {
    static VoiceSession s;
    return s;
}

VoiceState VoiceSession::state() const {
    std::lock_guard<std::mutex> lk(mu_);
    return state_;
}

void VoiceSession::ensure_recorder_locked_() {
    if (!recorder_) recorder_ = make_recorder();
}

void VoiceSession::start(const nlohmann::json& config) {
    std::lock_guard<std::mutex> lk(mu_);
    state_ = VoiceState::Listening;
    config_ = config;
    ensure_recorder_locked_();
}

void VoiceSession::stop() {
    std::lock_guard<std::mutex> lk(mu_);
    if (recorder_ && recorder_->is_recording()) {
        recorder_->cancel();
    }
    state_ = VoiceState::Inactive;
    last_recording_path_.clear();
}

nlohmann::json VoiceSession::status() const {
    std::lock_guard<std::mutex> lk(mu_);
    nlohmann::json j;
    j["state"] = state_string(state_);
    j["config"] = config_;
    j["recording"] = recorder_ && recorder_->is_recording();
    if (recorder_) {
        j["backend"] = recorder_->backend_name();
        j["elapsed_ms"] = recorder_->elapsed().count();
    }
    if (!last_recording_path_.empty()) {
        j["last_recording"] = last_recording_path_;
    }
    return j;
}

std::string VoiceSession::begin_recording(std::string& error) {
    std::lock_guard<std::mutex> lk(mu_);
    if (state_ == VoiceState::Inactive) {
        error = "voice session is inactive — call start first";
        return {};
    }
    ensure_recorder_locked_();
    if (recorder_->is_recording()) {
        error = "already recording";
        return {};
    }
    auto path = recorder_->start(error);
    if (!path.empty()) {
        last_recording_path_ = path;
    }
    return path;
}

std::string VoiceSession::finish_recording() {
    std::lock_guard<std::mutex> lk(mu_);
    if (!recorder_ || !recorder_->is_recording()) {
        return last_recording_path_;
    }
    auto path = recorder_->stop();
    if (!path.empty()) last_recording_path_ = path;
    return path;
}

std::string VoiceSession::transcribe_last(const std::string& language,
                                          const std::string& model,
                                          std::string& err) {
    // Stop recording first (if active), dispatch to transcription
    // backend, emit a `final` event.  We dispatch via the tool
    // registry so the shared backend injection hook is honoured.
    std::string audio_path;
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (recorder_ && recorder_->is_recording()) {
            audio_path = recorder_->stop();
        } else {
            audio_path = last_recording_path_;
        }
        if (audio_path.empty()) {
            err = "no recording available — call begin_recording first";
            return {};
        }
        last_recording_path_ = audio_path;
        state_ = VoiceState::Processing;
    }

    nlohmann::json args{{"audio_path", audio_path}};
    if (!language.empty()) args["language"] = language;
    if (!model.empty()) args["model_size"] = model;
    auto result_json =
        ToolRegistry::instance().dispatch("transcribe_audio", args, {});
    auto parsed = nlohmann::json::parse(result_json, nullptr, false);

    std::string text;
    std::string detected_lang;
    {
        std::lock_guard<std::mutex> lk(mu_);
        state_ = VoiceState::Listening;
        if (parsed.is_discarded() || parsed.contains("error")) {
            err = parsed.is_discarded()
                      ? std::string("transcription returned invalid JSON")
                      : parsed.value("error", std::string("transcription failed"));
            VoiceEvent ev{"error", err, "",
                          std::chrono::steady_clock::now()};
            events_.push_back(std::move(ev));
            return {};
        }
        text = parsed.value("text", std::string{});
        detected_lang = parsed.value("language", std::string{});
        VoiceEvent ev{"final", text, detected_lang,
                      std::chrono::steady_clock::now()};
        events_.push_back(std::move(ev));
    }
    return text;
}

std::vector<VoiceEvent> VoiceSession::drain_events() {
    std::lock_guard<std::mutex> lk(mu_);
    auto out = std::move(events_);
    events_.clear();
    return out;
}

void VoiceSession::reset() {
    std::lock_guard<std::mutex> lk(mu_);
    if (recorder_ && recorder_->is_recording()) {
        recorder_->cancel();
    }
    recorder_.reset();
    state_ = VoiceState::Inactive;
    config_ = nlohmann::json::object();
    last_recording_path_.clear();
    events_.clear();
}

std::string VoiceSession::state_string(VoiceState s) {
    switch (s) {
        case VoiceState::Inactive:   return "inactive";
        case VoiceState::Listening:  return "listening";
        case VoiceState::Processing: return "processing";
        case VoiceState::Speaking:   return "speaking";
    }
    return "unknown";
}

// ---------------------------------------------------------------------------
// Streaming pipeline — ffmpeg silencedetect → segment → transcribe →
// respond → TTS → playback.  Injection hooks live at namespace scope so
// tests can swap them for stubs.
// ---------------------------------------------------------------------------

namespace {

struct StreamingHooks {
    VoiceResponder responder;
    VoiceSpeaker speaker;
    VoicePlayback playback;
};

std::mutex& hooks_mutex() {
    static std::mutex m;
    return m;
}
StreamingHooks& hooks_slot() {
    static StreamingHooks h;
    return h;
}

StreamingHooks get_hooks() {
    std::lock_guard<std::mutex> lk(hooks_mutex());
    return hooks_slot();
}

// Default playback: try aplay (Linux), afplay (macOS), ffplay (cross).
bool default_playback(const std::string& audio_path) {
    hermes::environments::LocalEnvironment env;
    hermes::environments::ExecuteOptions opts;
    opts.timeout = std::chrono::seconds(60);
    auto try_bin = [&](const std::string& bin, const std::string& args) {
        auto check = env.execute("command -v " + bin + " 2>/dev/null", opts);
        if (check.exit_code != 0) return false;
        std::string cmd = bin + " " + args + " '" + audio_path + "' >/dev/null 2>&1";
        auto r = env.execute(cmd, opts);
        return r.exit_code == 0;
    };
    if (try_bin("aplay", "-q")) return true;
    if (try_bin("afplay", "")) return true;
    if (try_bin("ffplay", "-nodisp -autoexit -loglevel error")) return true;
    return false;
}

}  // namespace

void set_voice_responder_for_testing(VoiceResponder responder) {
    std::lock_guard<std::mutex> lk(hooks_mutex());
    hooks_slot().responder = std::move(responder);
}
void set_voice_speaker_for_testing(VoiceSpeaker speaker) {
    std::lock_guard<std::mutex> lk(hooks_mutex());
    hooks_slot().speaker = std::move(speaker);
}
void set_voice_playback_for_testing(VoicePlayback playback) {
    std::lock_guard<std::mutex> lk(hooks_mutex());
    hooks_slot().playback = std::move(playback);
}

StreamingPipelineResult run_streaming_pipeline(
    const StreamingPipelineConfig& cfg) {
    StreamingPipelineResult out;
    auto now = [] { return std::chrono::steady_clock::now(); };

    auto push_event = [&](const std::string& kind, const std::string& text,
                          const std::string& lang = "") {
        out.events.push_back(VoiceEvent{kind, text, lang, now()});
    };

    if (cfg.audio_path.empty()) {
        out.error = "audio_path required for streaming pipeline";
        push_event("error", out.error);
        return out;
    }

    // 1) Silence detection -> segment boundaries.
    ffmpeg::SilenceDetectOptions sil_opts;
    sil_opts.input_path = cfg.audio_path;
    sil_opts.noise_db = cfg.silence_noise_db;
    sil_opts.min_silence_seconds = cfg.silence_min_seconds;

    std::vector<ffmpeg::SilenceRegion> regions;
    auto sil = ffmpeg::detect_silence(sil_opts, regions);
    if (!sil.ok) {
        // Treat the entire file as one segment rather than aborting.
        regions.clear();
    }

    // Segments are the inverse of silence regions — cover [0,duration]
    // minus the silence intervals.  We don't need absolute duration —
    // the final segment ends "at EOF" which ffmpeg handles via -t
    // omitted / trim filter with `end` set to a large number.
    struct Segment {
        double start = 0.0;
        double end = 0.0;  // 0.0 = open (EOF)
    };
    std::vector<Segment> segments;
    double cursor = 0.0;
    for (const auto& reg : regions) {
        if (reg.start_seconds - cursor > cfg.min_segment_seconds) {
            segments.push_back({cursor, reg.start_seconds});
        }
        cursor = reg.end_seconds;
    }
    // Tail segment from last silence to EOF.
    segments.push_back({cursor, 0.0});

    if (segments.empty()) {
        // Fallback: the whole file.
        segments.push_back({0.0, 0.0});
    }

    auto hooks = get_hooks();

    // 2) For each segment, slice via ffmpeg to a temp WAV and dispatch
    // to transcribe_audio.  Emit `partial` on slice, `final` on text.
    auto tmpdir = std::filesystem::temp_directory_path();
    for (std::size_t idx = 0; idx < segments.size(); ++idx) {
        const auto& seg = segments[idx];
        auto seg_path = tmpdir /
            ("hermes_voice_seg_" + std::to_string(::getpid()) + "_" +
             std::to_string(idx) + ".wav");

        std::vector<std::string> argv = {
            "-y", "-hide_banner", "-loglevel", "error",
            "-i", cfg.audio_path,
            "-ss", std::to_string(seg.start),
        };
        if (seg.end > 0.0 && seg.end > seg.start) {
            argv.push_back("-to");
            argv.push_back(std::to_string(seg.end));
        }
        argv.insert(argv.end(),
                    {"-ar", "16000", "-ac", "1",
                     "-c:a", "pcm_s16le", "-f", "wav",
                     seg_path.string()});
        auto slice = ffmpeg::run_ffmpeg(argv);
        if (!slice.ok) {
            push_event("error",
                       "failed to slice segment " + std::to_string(idx) +
                           ": " + slice.error);
            continue;
        }
        push_event("partial", "segment " + std::to_string(idx) + " ready");

        nlohmann::json args{{"audio_path", seg_path.string()}};
        if (!cfg.language.empty()) args["language"] = cfg.language;
        if (!cfg.model.empty()) args["model_size"] = cfg.model;
        auto result_json =
            ToolRegistry::instance().dispatch("transcribe_audio", args, {});
        auto parsed = nlohmann::json::parse(result_json, nullptr, false);
        std::error_code ec;
        std::filesystem::remove(seg_path, ec);

        if (parsed.is_discarded() || parsed.contains("error")) {
            push_event("error",
                       parsed.is_discarded()
                           ? std::string("transcription returned invalid JSON")
                           : parsed.value("error",
                                          std::string("transcription failed")));
            continue;
        }
        std::string text = parsed.value("text", std::string{});
        std::string lang = parsed.value("language", std::string{});
        if (text.empty()) continue;
        push_event("final", text, lang);

        // 3) Responder.
        if (!hooks.responder) continue;
        std::string response = hooks.responder(text, lang);
        if (response.empty()) continue;
        push_event("response", response, lang);

        // 4) TTS + playback.
        if (!cfg.speak_response) continue;
        if (!hooks.speaker) continue;
        std::string tts_err;
        std::string audio_out = hooks.speaker(response, cfg.tts_voice, tts_err);
        if (audio_out.empty()) {
            push_event("error",
                       "tts failed: " +
                           (tts_err.empty() ? "no output" : tts_err));
            continue;
        }
        push_event("speaking", audio_out);

        if (cfg.playback_response) {
            auto play = hooks.playback ? hooks.playback : &default_playback;
            if (!play(audio_out)) {
                push_event("error", "playback failed for " + audio_out);
            }
        }
    }

    push_event("done", "streaming pipeline finished");
    out.ok = true;
    return out;
}

// ---------------------------------------------------------------------------
// Tool handler
// ---------------------------------------------------------------------------

namespace {

nlohmann::json events_to_json(const std::vector<VoiceEvent>& evs) {
    auto arr = nlohmann::json::array();
    for (const auto& e : evs) {
        arr.push_back({{"kind", e.kind},
                       {"text", e.text},
                       {"language", e.language}});
    }
    return arr;
}

std::string handle_voice_mode(const nlohmann::json& args,
                              const ToolContext& /*ctx*/) {
    if (!args.contains("action") || !args["action"].is_string()) {
        return tool_error("missing required string field `action`");
    }
    const auto action = args["action"].get<std::string>();
    auto& session = VoiceSession::instance();

    if (action == "start") {
        nlohmann::json config = args.value("config", nlohmann::json::object());
        session.start(config);
        return tool_result({{"started", true}, {"state", "listening"}});
    }

    if (action == "stop") {
        session.stop();
        return tool_result({{"stopped", true}, {"state", "inactive"}});
    }

    if (action == "status") {
        return tool_result(session.status());
    }

    if (action == "record") {
        std::string err;
        auto path = session.begin_recording(err);
        if (path.empty()) {
            return tool_error(err.empty() ? "could not start recording" : err);
        }
        return tool_result({{"recording", true}, {"path", path}});
    }

    if (action == "transcribe") {
        std::string lang = args.value("language", std::string{});
        std::string model = args.value("model_size", std::string{});
        if (model.empty()) model = args.value("model", std::string{});
        std::string err;
        auto text = session.transcribe_last(lang, model, err);
        if (!err.empty()) return tool_error(err);
        auto events = session.drain_events();
        nlohmann::json out;
        out["text"] = text;
        out["events"] = events_to_json(events);
        return tool_result(out);
    }

    if (action == "events") {
        auto events = session.drain_events();
        return tool_result({{"events", events_to_json(events)}});
    }

    if (action == "streaming" || action == "stream") {
        StreamingPipelineConfig cfg;
        cfg.audio_path = args.value("audio_path", std::string{});
        // If not given, fall back to the last session recording.
        if (cfg.audio_path.empty()) {
            auto status = session.status();
            cfg.audio_path = status.value("last_recording", std::string{});
        }
        if (cfg.audio_path.empty()) {
            return tool_error(
                "streaming mode requires audio_path or a prior recording");
        }
        cfg.language = args.value("language", std::string{});
        cfg.model = args.value("model_size", std::string{});
        if (cfg.model.empty()) cfg.model = args.value("model", std::string{});
        cfg.tts_voice = args.value("tts_voice", std::string{});
        cfg.tts_provider = args.value("tts_provider", std::string{});
        cfg.silence_noise_db =
            args.value("silence_noise_db", cfg.silence_noise_db);
        cfg.silence_min_seconds =
            args.value("silence_min_seconds", cfg.silence_min_seconds);
        cfg.speak_response = args.value("speak_response", true);
        cfg.playback_response = args.value("playback_response", false);

        auto result = run_streaming_pipeline(cfg);
        nlohmann::json out;
        out["ok"] = result.ok;
        out["events"] = events_to_json(result.events);
        if (!result.error.empty()) out["error"] = result.error;
        return tool_result(out);
    }

    return tool_error(
        "unknown voice_mode action '" + action +
        "' — expected start|stop|status|record|transcribe|events|streaming");
}

}  // namespace

// ---------------------------------------------------------------------------
// Registration
// ---------------------------------------------------------------------------

void register_voice_tools() {
    auto& reg = ToolRegistry::instance();

    ToolEntry e;
    e.name = "voice_mode";
    e.toolset = "voice";
    e.description = "Manage voice input/output session (push-to-talk "
                    "capture + transcription).";
    e.emoji = "\xF0\x9F\x8E\xA4";  // microphone2
    e.schema = {
        {"type", "object"},
        {"properties",
         {{"action",
           {{"type", "string"},
            {"enum", nlohmann::json::array({"start", "stop", "status",
                                            "record", "transcribe",
                                            "events", "streaming",
                                            "stream"})},
            {"description",
             "start|stop session; record begins capture; transcribe "
             "finishes capture and returns text; events drains pending "
             "partial/final/response/speaking/done/error events; "
             "streaming runs the full STT→LLM→TTS pipeline on an "
             "audio_path (or the last recording)"}}},
          {"config",
           {{"type", "object"},
            {"description",
             "Optional session config: {stt_model, tts_voice, tts_provider}"}}},
          {"language",
           {{"type", "string"},
            {"description",
             "ISO-639-1 language code for transcribe; 'auto' to detect"}}},
          {"model_size",
           {{"type", "string"},
            {"description",
             "Whisper model size for transcribe (tiny|base|small|medium|large)"}}}}},
        {"required", nlohmann::json::array({"action"})}};
    e.handler = handle_voice_mode;
    reg.register_tool(std::move(e));
}

}  // namespace hermes::tools
