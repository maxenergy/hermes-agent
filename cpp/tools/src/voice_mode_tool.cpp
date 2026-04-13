// voice_mode tool — session state machine + shell-driven capture.

#include "hermes/tools/voice_mode_tool.hpp"

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
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

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
    }

    std::string stop() override {
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
    pid_t pid_ = -1;
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

    return tool_error("unknown voice_mode action '" + action +
                      "' — expected start|stop|status|record|transcribe|events");
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
                                            "events"})},
            {"description",
             "start|stop session; record begins capture; transcribe "
             "finishes capture and returns text; events drains pending "
             "partial/final/error events"}}},
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
