// Transcription tool — whisper.cpp CLI subprocess backend.
//
// Tries (in order) `whisper-cli`, `whisper-cpp`, `main` (the whisper.cpp
// default binary name), then falls back to the Python `whisper` /
// `faster-whisper` CLIs.  All variants share the same CLI shape enough
// that we can feed them a minimal "--model X --language Y FILE"
// invocation and parse either stdout or a sibling ".txt"/".json" file.
//
// whisper.cpp integration: we intentionally shell out rather than link
// the whisper.cpp C API directly — that would drag in ggml, a large
// model blob at build time, and a BLAS backend choice across three OSes.
// The CLI route matches the Python reference implementation's behaviour
// (which also shells out) and keeps the unit test surface lightweight.
// If a downstream deployment wants first-party whisper.cpp, it can set
// the backend via `set_transcription_backend_for_testing` (misnamed, but
// the injection point is general).

#include "hermes/tools/transcription_tool.hpp"

#include "hermes/tools/registry.hpp"

#include "hermes/environments/local.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

namespace hermes::tools {

namespace {

const std::vector<std::string> kAudioExtensions = {
    ".mp3", ".wav", ".ogg", ".flac", ".m4a", ".webm", ".mp4", ".mpeg",
    ".mpga", ".oga"};

bool is_audio_extension(std::string ext) {
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    return std::find(kAudioExtensions.begin(), kAudioExtensions.end(), ext) !=
           kAudioExtensions.end();
}

// Shell-quote a single argv token.  We keep it conservative: if the
// string contains only [A-Za-z0-9_./:=-] we pass it raw; otherwise wrap
// in single quotes and escape embedded quotes.
std::string shell_quote(const std::string& s) {
    bool safe = !s.empty();
    for (char c : s) {
        if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_' ||
              c == '.' || c == '/' || c == ':' || c == '=' || c == '-')) {
            safe = false;
            break;
        }
    }
    if (safe) return s;
    std::string out = "'";
    for (char c : s) {
        if (c == '\'') out += "'\\''";
        else out += c;
    }
    out += "'";
    return out;
}

// Look up a binary in $PATH.  Returns the absolute path, or empty
// string if not found.  Uses the `command -v` builtin via /bin/sh so
// we don't need to re-implement PATH scanning ourselves.
std::string which(const std::string& bin) {
    hermes::environments::LocalEnvironment env;
    hermes::environments::ExecuteOptions opts;
    opts.timeout = std::chrono::seconds(5);
    auto r = env.execute("command -v " + shell_quote(bin) + " 2>/dev/null",
                         opts);
    if (r.exit_code != 0) return {};
    std::string out = r.stdout_text;
    // strip trailing newline(s)
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r'))
        out.pop_back();
    return out;
}

// Find a whisper-compatible binary on $PATH.  Returns {binary, flavour}
// where flavour is "whisper-cpp" or "whisper-py".
struct WhisperBinary {
    std::string path;
    std::string flavour;   // "whisper-cpp" | "whisper-py" | "faster-whisper"
    std::string name;      // displayed backend name in output JSON
};

WhisperBinary find_whisper_binary() {
    // whisper.cpp flavours — `whisper-cli` is the modern build target,
    // `whisper-cpp` is the Homebrew/vcpkg package alias, `main` is the
    // legacy in-tree binary.  We skip `main` unless its parent dir
    // looks whisper-ish (too generic a name), instead preferring the
    // first two.
    for (const auto& cand : {"whisper-cli", "whisper-cpp"}) {
        auto p = which(cand);
        if (!p.empty()) return {p, "whisper-cpp", cand};
    }
    // Python whisper
    if (auto p = which("whisper"); !p.empty()) {
        return {p, "whisper-py", "whisper"};
    }
    if (auto p = which("faster-whisper"); !p.empty()) {
        return {p, "faster-whisper", "faster-whisper"};
    }
    return {};
}

// ---------------------------------------------------------------------------
// Default backend — subprocess to whisper CLI.
// ---------------------------------------------------------------------------

TranscriptionResult run_subprocess_backend(const TranscriptionRequest& req) {
    TranscriptionResult out;
    auto bin = find_whisper_binary();
    if (bin.path.empty()) {
        out.error =
            "no whisper binary found in PATH — install whisper.cpp "
            "(provides `whisper-cli`), or the Python `whisper` / "
            "`faster-whisper` CLI";
        return out;
    }

    hermes::environments::LocalEnvironment env;
    hermes::environments::ExecuteOptions opts;
    opts.timeout = std::chrono::seconds(600);

    std::ostringstream cmd;
    cmd << shell_quote(bin.path);

    const std::string model = req.model.empty() ? "base" : req.model;

    if (bin.flavour == "whisper-cpp") {
        // whisper-cli usage: whisper-cli -m MODEL_PATH -l LANG -f AUDIO
        //
        // whisper-cli requires a .bin model file, not a name.  We look
        // in $WHISPER_MODEL_PATH / $WHISPER_CPP_MODELS / current dir /
        // ~/.cache/whisper.cpp for `ggml-<model>.bin`.
        std::string model_file;
        std::vector<std::string> search_dirs;
        if (const char* p = std::getenv("WHISPER_MODEL_PATH")) {
            search_dirs.emplace_back(p);
        }
        if (const char* p = std::getenv("WHISPER_CPP_MODELS")) {
            search_dirs.emplace_back(p);
        }
        if (const char* home = std::getenv("HOME")) {
            search_dirs.emplace_back(std::string(home) + "/.cache/whisper.cpp");
            search_dirs.emplace_back(std::string(home) +
                                     "/.local/share/whisper.cpp");
        }
        search_dirs.emplace_back(".");
        for (const auto& dir : search_dirs) {
            auto candidate =
                std::filesystem::path(dir) / ("ggml-" + model + ".bin");
            std::error_code ec;
            if (std::filesystem::exists(candidate, ec)) {
                model_file = candidate.string();
                break;
            }
        }
        if (model_file.empty()) {
            out.error =
                "whisper.cpp model file `ggml-" + model +
                ".bin` not found — set $WHISPER_MODEL_PATH or place the "
                "file under ~/.cache/whisper.cpp/";
            return out;
        }
        cmd << " -m " << shell_quote(model_file);
        if (req.language != "auto" && !req.language.empty()) {
            cmd << " -l " << shell_quote(req.language);
        }
        cmd << " -nt -np ";  // no-timestamps, no-prints (stdout = transcript)
        cmd << " -f " << shell_quote(req.audio_path);
    } else if (bin.flavour == "whisper-py") {
        cmd << " --model " << shell_quote(model);
        if (req.language != "auto" && !req.language.empty()) {
            cmd << " --language " << shell_quote(req.language);
        }
        cmd << " --output_format json --output_dir "
            << shell_quote(std::filesystem::temp_directory_path().string())
            << " " << shell_quote(req.audio_path);
    } else {  // faster-whisper
        cmd << " --model " << shell_quote(model);
        if (req.language != "auto" && !req.language.empty()) {
            cmd << " --language " << shell_quote(req.language);
        }
        cmd << " --output_format json " << shell_quote(req.audio_path);
    }

    auto result = env.execute(cmd.str(), opts);
    if (result.exit_code != 0) {
        out.error = std::string("whisper backend `") + bin.name +
                    "` failed (exit " + std::to_string(result.exit_code) +
                    "): " + result.stdout_text;
        return out;
    }

    out.ok = true;
    out.backend = bin.name;
    out.language = req.language;

    // whisper-cpp -nt -np prints the transcript to stdout directly.
    if (bin.flavour == "whisper-cpp") {
        out.text = result.stdout_text;
        // trim leading/trailing whitespace
        auto trim = [](std::string& s) {
            while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front())))
                s.erase(s.begin());
            while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back())))
                s.pop_back();
        };
        trim(out.text);
        return out;
    }

    // whisper-py / faster-whisper write a JSON file next to the audio
    // (or in --output_dir).  Try to parse the stdout as JSON first, and
    // if that fails fall back to the raw text.
    auto parsed = nlohmann::json::parse(result.stdout_text, nullptr, false);
    if (!parsed.is_discarded() && parsed.is_object()) {
        if (parsed.contains("text")) {
            out.text = parsed["text"].get<std::string>();
        }
        if (parsed.contains("language")) {
            out.language = parsed["language"].get<std::string>();
        }
        if (parsed.contains("segments")) {
            out.segments = parsed["segments"];
        }
        return out;
    }

    // Look for a sibling .json file in tmp dir (whisper-py default).
    auto stem = std::filesystem::path(req.audio_path).stem().string();
    auto tmp_dir = std::filesystem::temp_directory_path();
    auto json_path = tmp_dir / (stem + ".json");
    std::error_code ec;
    if (std::filesystem::exists(json_path, ec)) {
        std::ifstream f(json_path);
        auto sidecar = nlohmann::json::parse(f, nullptr, false);
        if (!sidecar.is_discarded() && sidecar.is_object()) {
            if (sidecar.contains("text")) {
                out.text = sidecar["text"].get<std::string>();
            }
            if (sidecar.contains("language")) {
                out.language = sidecar["language"].get<std::string>();
            }
            if (sidecar.contains("segments")) {
                out.segments = sidecar["segments"];
            }
            return out;
        }
    }

    // Last resort: stdout is plain text.
    out.text = result.stdout_text;
    return out;
}

// ---------------------------------------------------------------------------
// Backend dispatcher — allows tests to inject a mock.
// ---------------------------------------------------------------------------

std::mutex& backend_mutex() {
    static std::mutex m;
    return m;
}

TranscriptionBackend& backend_slot() {
    static TranscriptionBackend b;
    return b;
}

TranscriptionResult dispatch_backend(const TranscriptionRequest& req) {
    TranscriptionBackend cb;
    {
        std::lock_guard<std::mutex> lk(backend_mutex());
        cb = backend_slot();
    }
    if (cb) return cb(req);
    return run_subprocess_backend(req);
}

// ---------------------------------------------------------------------------
// Tool handler
// ---------------------------------------------------------------------------

std::string handle_transcribe(const nlohmann::json& args,
                              const ToolContext& /*ctx*/) {
    if (!args.contains("audio_path") || !args["audio_path"].is_string()) {
        return tool_error("missing required string field `audio_path`");
    }
    TranscriptionRequest req;
    req.audio_path = args["audio_path"].get<std::string>();
    req.language = args.contains("language") && args["language"].is_string()
                       ? args["language"].get<std::string>()
                       : std::string("auto");
    // Accept either `model` or `model_size` for parity with the Python
    // reference tool, which uses `model_size`.
    if (args.contains("model_size") && args["model_size"].is_string()) {
        req.model = args["model_size"].get<std::string>();
    } else if (args.contains("model") && args["model"].is_string()) {
        req.model = args["model"].get<std::string>();
    } else {
        req.model = "base";
    }

    std::error_code ec;
    if (!std::filesystem::exists(req.audio_path, ec)) {
        return tool_error("audio file not found: " + req.audio_path);
    }

    auto ext = std::filesystem::path(req.audio_path).extension().string();
    if (!is_audio_extension(ext)) {
        return tool_error(
            "unsupported audio format '" + ext +
            "' — supported: .mp3, .wav, .ogg, .flac, .m4a, .webm, .mp4, "
            ".mpeg, .mpga, .oga");
    }

    auto result = dispatch_backend(req);
    if (!result.ok) {
        return tool_error(result.error);
    }

    nlohmann::json r;
    r["text"] = result.text;
    r["language"] = result.language;
    r["model"] = req.model;
    r["backend"] = result.backend;
    if (!result.segments.is_null() && !result.segments.empty()) {
        r["segments"] = result.segments;
    }
    return tool_result(r);
}

}  // namespace

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void set_transcription_backend_for_testing(TranscriptionBackend backend) {
    std::lock_guard<std::mutex> lk(backend_mutex());
    backend_slot() = std::move(backend);
}

void register_transcription_tools() {
    auto& reg = ToolRegistry::instance();

    ToolEntry e;
    e.name = "transcribe_audio";
    e.toolset = "voice";
    e.description =
        "Transcribe an audio file to text using a local whisper.cpp / "
        "whisper / faster-whisper CLI backend.";
    e.emoji = "\xF0\x9F\x8E\x99";  // microphone
    e.schema = {
        {"type", "object"},
        {"properties",
         {{"audio_path",
           {{"type", "string"},
            {"description", "Path to the audio file to transcribe"}}},
          {"model_size",
           {{"type", "string"},
            {"description",
             "Whisper model size (tiny|base|small|medium|large), default base"}}},
          {"language",
           {{"type", "string"},
            {"description",
             "ISO-639-1 language code (e.g. en, zh); 'auto' to detect"}}}}},
        {"required", nlohmann::json::array({"audio_path"})}};
    e.handler = handle_transcribe;
    reg.register_tool(std::move(e));
}

}  // namespace hermes::tools
