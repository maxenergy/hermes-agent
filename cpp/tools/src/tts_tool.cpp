// Text-to-speech tool — depth port of ``tools/tts_tool.py``.
//
// Providers:
//   edge        — CLI shell-out to edge-tts (default, no API key)
//   elevenlabs  — HTTPS POST to api.elevenlabs.io
//   openai      — HTTPS POST to api.openai.com/v1/audio/speech
//   minimax     — HTTPS POST to api.minimax.io (hex-encoded response)
//   mistral     — HTTPS POST to api.mistral.ai/v1/audio/speech/complete
//                 (base64-encoded response)
//   neutts      — local neutts CLI shell-out
//
// The C++ port keeps the lazy, fail-soft posture of the Python module:
// missing providers do not crash; they return ``{"error": "..."}``
// envelopes instead.

#include "hermes/tools/tts_tool.hpp"
#include "hermes/tools/registry.hpp"

#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <unordered_map>

namespace hermes::tools {

namespace {

// ---------------------------------------------------------------------------
// Generic helpers (shell escape, PATH lookup, env var reads).
// ---------------------------------------------------------------------------

std::string shell_escape(const std::string& s) {
    std::string out = "'";
    for (char c : s) {
        if (c == '\'') out += "'\\''";
        else out += c;
    }
    out += "'";
    return out;
}

std::string getenv_str(const char* key) {
    const char* v = std::getenv(key);
    return v ? std::string(v) : std::string();
}

bool path_has_executable(const std::string& bin) {
    if (bin.find('/') != std::string::npos) {
        return ::access(bin.c_str(), X_OK) == 0;
    }
    const char* pe = std::getenv("PATH");
    if (!pe) return false;
    std::string cur;
    auto check = [&](const std::string& dir) {
        if (dir.empty()) return false;
        return ::access((dir + "/" + bin).c_str(), X_OK) == 0;
    };
    for (const char* c = pe; *c; ++c) {
        if (*c == ':') {
            if (check(cur)) return true;
            cur.clear();
        } else {
            cur.push_back(*c);
        }
    }
    return check(cur);
}

std::string ends_with_ext(const std::string& path) {
    auto dot = path.rfind('.');
    if (dot == std::string::npos) return {};
    std::string ext = path.substr(dot);
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    return ext;
}

void write_bytes(const std::string& path, const std::string& bytes) {
    std::ofstream ofs(path, std::ios::binary);
    if (!ofs) return;
    ofs.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

}  // namespace

// ---------------------------------------------------------------------------
// openai_response_format_for / elevenlabs_output_format_for / ...
// ---------------------------------------------------------------------------
std::string openai_response_format_for(const std::string& output_path) {
    auto ext = ends_with_ext(output_path);
    if (ext == ".ogg" || ext == ".opus") return "opus";
    if (ext == ".wav") return "wav";
    if (ext == ".flac") return "flac";
    if (ext == ".aac") return "aac";
    if (ext == ".pcm") return "pcm";
    return "mp3";
}

std::string elevenlabs_output_format_for(const std::string& output_path) {
    auto ext = ends_with_ext(output_path);
    if (ext == ".ogg" || ext == ".opus") return "opus_48000_64";
    if (ext == ".pcm") return "pcm_44100";
    return "mp3_44100_128";
}

std::string minimax_audio_format_for(const std::string& output_path) {
    auto ext = ends_with_ext(output_path);
    if (ext == ".wav") return "wav";
    if (ext == ".flac") return "flac";
    return "mp3";
}

std::string mistral_response_format_for(const std::string& output_path) {
    auto ext = ends_with_ext(output_path);
    if (ext == ".ogg" || ext == ".opus") return "opus";
    if (ext == ".wav") return "wav";
    if (ext == ".flac") return "flac";
    return "mp3";
}

// ---------------------------------------------------------------------------
// has_ffmpeg / convert_to_opus
// ---------------------------------------------------------------------------

bool has_ffmpeg() {
    static std::once_flag once;
    static bool cached = false;
    std::call_once(once, [] { cached = path_has_executable("ffmpeg"); });
    return cached;
}

std::string convert_to_opus(const std::string& mp3_path) {
    if (!has_ffmpeg()) return {};
    if (mp3_path.empty()) return {};
    auto dot = mp3_path.rfind('.');
    std::string base =
        (dot == std::string::npos) ? mp3_path : mp3_path.substr(0, dot);
    std::string ogg = base + ".ogg";
    std::ostringstream cmd;
    cmd << "ffmpeg -i " << shell_escape(mp3_path)
        << " -acodec libopus -ac 1 -b:a 64k -vbr off "
        << shell_escape(ogg) << " -y 2>/dev/null";
    int rc = std::system(cmd.str().c_str());
    if (rc != 0) return {};
    std::error_code ec;
    if (!std::filesystem::exists(ogg, ec)) return {};
    auto size = std::filesystem::file_size(ogg, ec);
    if (ec || size == 0) return {};
    return ogg;
}

// ---------------------------------------------------------------------------
// strip_markdown_for_tts
// ---------------------------------------------------------------------------
std::string strip_markdown_for_tts(const std::string& text) {
    std::string s = text;
    auto sub_loop = [](std::string in, const std::string& open,
                       const std::string& close, const std::string& replace) {
        std::string out;
        std::size_t i = 0;
        while (i < in.size()) {
            auto o = in.find(open, i);
            if (o == std::string::npos) {
                out.append(in, i, std::string::npos);
                break;
            }
            out.append(in, i, o - i);
            auto c = in.find(close, o + open.size());
            if (c == std::string::npos) {
                out.append(in, o, std::string::npos);
                break;
            }
            out += replace;
            i = c + close.size();
        }
        return out;
    };

    // Code fences  ```…```  -> single space.
    s = sub_loop(s, "```", "```", " ");
    // Inline code `…` -> inner text (without backticks).
    {
        std::string out;
        for (std::size_t i = 0; i < s.size(); ++i) {
            if (s[i] == '`') {
                auto end = s.find('`', i + 1);
                if (end == std::string::npos) {
                    out.append(s, i, std::string::npos);
                    break;
                }
                out.append(s, i + 1, end - i - 1);
                i = end;
            } else {
                out.push_back(s[i]);
            }
        }
        s = std::move(out);
    }
    // Markdown links [text](url) -> text.
    {
        std::string out;
        for (std::size_t i = 0; i < s.size();) {
            if (s[i] == '[') {
                auto rb = s.find(']', i + 1);
                if (rb != std::string::npos && rb + 1 < s.size() &&
                    s[rb + 1] == '(') {
                    auto rp = s.find(')', rb + 2);
                    if (rp != std::string::npos) {
                        out.append(s, i + 1, rb - i - 1);
                        i = rp + 1;
                        continue;
                    }
                }
            }
            out.push_back(s[i++]);
        }
        s = std::move(out);
    }
    // Plain URLs — drop http(s)://… tokens.
    {
        std::string out;
        for (std::size_t i = 0; i < s.size();) {
            if (s.compare(i, 7, "http://") == 0 ||
                s.compare(i, 8, "https://") == 0) {
                while (i < s.size() && !std::isspace(
                           static_cast<unsigned char>(s[i]))) {
                    ++i;
                }
            } else {
                out.push_back(s[i++]);
            }
        }
        s = std::move(out);
    }
    // **bold** and *italic* and _emphasis_.
    auto strip_markers = [](std::string in, char marker) {
        std::string out;
        bool open = false;
        for (std::size_t i = 0; i < in.size(); ++i) {
            if (in[i] == marker &&
                (i + 1 < in.size() && in[i + 1] == marker)) {
                open = !open;
                ++i;
                continue;
            }
            if (in[i] == marker) {
                open = !open;
                continue;
            }
            out.push_back(in[i]);
        }
        return out;
    };
    s = strip_markers(s, '*');
    s = strip_markers(s, '_');

    // Headers `^#+\s*`  and list bullets `^\s*[-*]\s+` — line-oriented.
    {
        std::ostringstream os;
        std::istringstream is(s);
        std::string line;
        while (std::getline(is, line)) {
            std::size_t start = 0;
            while (start < line.size() &&
                   (line[start] == ' ' || line[start] == '\t')) {
                ++start;
            }
            // Headers.
            std::size_t h = start;
            while (h < line.size() && line[h] == '#') ++h;
            if (h > start) {
                while (h < line.size() && line[h] == ' ') ++h;
                os << line.substr(h) << '\n';
                continue;
            }
            // List bullets.
            if (start < line.size() &&
                (line[start] == '-' || line[start] == '*')) {
                std::size_t q = start + 1;
                if (q < line.size() && line[q] == ' ') {
                    os << line.substr(q + 1) << '\n';
                    continue;
                }
            }
            // Horizontal rules `---+`
            std::size_t d = start;
            while (d < line.size() && line[d] == '-') ++d;
            if (d - start >= 3 && d == line.size()) continue;
            os << line << '\n';
        }
        s = os.str();
    }
    // Collapse 3+ newlines to 2.
    {
        std::string out;
        int nl = 0;
        for (char c : s) {
            if (c == '\n') {
                ++nl;
                if (nl <= 2) out.push_back(c);
            } else {
                nl = 0;
                out.push_back(c);
            }
        }
        s = std::move(out);
    }
    // Trim.
    auto is_ws = [](unsigned char c) { return std::isspace(c); };
    while (!s.empty() && is_ws(s.front())) s.erase(s.begin());
    while (!s.empty() && is_ws(s.back())) s.pop_back();
    return s;
}

// ---------------------------------------------------------------------------
// split_sentences
// ---------------------------------------------------------------------------
std::vector<std::string> split_sentences(const std::string& text) {
    std::vector<std::string> out;
    std::string cur;
    for (std::size_t i = 0; i < text.size(); ++i) {
        cur.push_back(text[i]);
        bool term = (text[i] == '.' || text[i] == '!' || text[i] == '?');
        bool next_boundary = false;
        if (term && i + 1 < text.size()) {
            char n = text[i + 1];
            if (n == ' ' || n == '\n' || n == '\t') next_boundary = true;
        } else if (text[i] == '\n' && i + 1 < text.size() &&
                   text[i + 1] == '\n') {
            next_boundary = true;
        }
        if (next_boundary) {
            auto trimmed = cur;
            while (!trimmed.empty() &&
                   std::isspace(static_cast<unsigned char>(trimmed.back()))) {
                trimmed.pop_back();
            }
            if (!trimmed.empty()) out.push_back(trimmed);
            cur.clear();
        }
    }
    // Flush remainder.
    while (!cur.empty() &&
           std::isspace(static_cast<unsigned char>(cur.front()))) {
        cur.erase(cur.begin());
    }
    while (!cur.empty() &&
           std::isspace(static_cast<unsigned char>(cur.back()))) {
        cur.pop_back();
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

// ---------------------------------------------------------------------------
// cap_text_length
// ---------------------------------------------------------------------------
std::string cap_text_length(const std::string& text, std::size_t max_chars) {
    if (text.size() <= max_chars) return text;
    return text.substr(0, max_chars);
}

// ---------------------------------------------------------------------------
// decode_hex / decode_base64
// ---------------------------------------------------------------------------

std::optional<std::string> decode_hex(const std::string& hex) {
    if (hex.size() % 2 != 0) return std::nullopt;
    auto digit = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return 10 + c - 'a';
        if (c >= 'A' && c <= 'F') return 10 + c - 'A';
        return -1;
    };
    std::string out;
    out.reserve(hex.size() / 2);
    for (std::size_t i = 0; i < hex.size(); i += 2) {
        int hi = digit(hex[i]);
        int lo = digit(hex[i + 1]);
        if (hi < 0 || lo < 0) return std::nullopt;
        out.push_back(static_cast<char>((hi << 4) | lo));
    }
    return out;
}

std::optional<std::string> decode_base64(const std::string& b64) {
    static const int8_t table[256] = [] {
        int8_t t[256];
        for (auto& v : t) v = -1;
        const char* alph =
            "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        for (int i = 0; i < 64; ++i) {
            t[static_cast<unsigned char>(alph[i])] = static_cast<int8_t>(i);
        }
        t[static_cast<unsigned char>('=')] = -2;
        return std::to_array(t);
    }()
    .data();
    std::string out;
    out.reserve(b64.size() * 3 / 4);
    int buf = 0;
    int bits = 0;
    for (unsigned char c : b64) {
        if (std::isspace(c)) continue;
        int v = table[c];
        if (v == -2) break;            // padding
        if (v == -1) return std::nullopt;
        buf = (buf << 6) | v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<char>((buf >> bits) & 0xFF));
        }
    }
    return out;
}

// ---------------------------------------------------------------------------
// Voice lists — small, static, covering the default shipping voices.
// ---------------------------------------------------------------------------
const std::vector<TtsVoice>& edge_voices() {
    static const std::vector<TtsVoice> voices = {
        {"en-US-AriaNeural", "Aria", "en-US", "female"},
        {"en-US-GuyNeural", "Guy", "en-US", "male"},
        {"en-US-JennyNeural", "Jenny", "en-US", "female"},
        {"en-US-DavisNeural", "Davis", "en-US", "male"},
        {"en-GB-SoniaNeural", "Sonia", "en-GB", "female"},
        {"en-GB-RyanNeural", "Ryan", "en-GB", "male"},
        {"en-AU-NatashaNeural", "Natasha", "en-AU", "female"},
        {"en-IN-PrabhatNeural", "Prabhat", "en-IN", "male"},
        {"de-DE-KatjaNeural", "Katja", "de-DE", "female"},
        {"fr-FR-DeniseNeural", "Denise", "fr-FR", "female"},
        {"es-ES-ElviraNeural", "Elvira", "es-ES", "female"},
        {"ja-JP-NanamiNeural", "Nanami", "ja-JP", "female"},
        {"zh-CN-XiaoxiaoNeural", "Xiaoxiao", "zh-CN", "female"},
    };
    return voices;
}

const std::vector<TtsVoice>& openai_voices() {
    static const std::vector<TtsVoice> voices = {
        {"alloy", "Alloy", "en", "neutral"},
        {"ash", "Ash", "en", "neutral"},
        {"ballad", "Ballad", "en", "neutral"},
        {"coral", "Coral", "en", "female"},
        {"echo", "Echo", "en", "male"},
        {"fable", "Fable", "en", "male"},
        {"onyx", "Onyx", "en", "male"},
        {"nova", "Nova", "en", "female"},
        {"sage", "Sage", "en", "neutral"},
        {"shimmer", "Shimmer", "en", "female"},
    };
    return voices;
}

const std::vector<TtsVoice>& elevenlabs_default_voices() {
    static const std::vector<TtsVoice> voices = {
        {"21m00Tcm4TlvDq8ikWAM", "Rachel", "en-US", "female"},
        {"pNInz6obpgDQGcFmaJgB", "Adam", "en-US", "male"},
        {"EXAVITQu4vr4xnSDxMaL", "Bella", "en-US", "female"},
        {"ErXwobaYiN019PkySvjV", "Antoni", "en-US", "male"},
        {"TxGEqnHWrfWFTfGW9XjX", "Josh", "en-US", "male"},
        {"VR6AewLTigWG4xSOukaG", "Arnold", "en-US", "male"},
        {"yoZ06aMxZJJ28mfd3POQ", "Sam", "en-US", "male"},
    };
    return voices;
}

// ---------------------------------------------------------------------------
// tts_provider_unavailable_reason
// ---------------------------------------------------------------------------
std::string tts_provider_unavailable_reason(const std::string& provider) {
    if (provider == "edge") {
        if (!path_has_executable("edge-tts")) {
            return "edge-tts binary not found — install with pip install edge-tts";
        }
        return {};
    }
    if (provider == "openai") {
        if (getenv_str("OPENAI_API_KEY").empty() &&
            getenv_str("VOICE_TOOLS_OPENAI_KEY").empty()) {
            return "OPENAI_API_KEY not set";
        }
        return {};
    }
    if (provider == "elevenlabs") {
        if (getenv_str("ELEVENLABS_API_KEY").empty()) {
            return "ELEVENLABS_API_KEY not set";
        }
        return {};
    }
    if (provider == "minimax") {
        if (getenv_str("MINIMAX_API_KEY").empty()) {
            return "MINIMAX_API_KEY not set";
        }
        return {};
    }
    if (provider == "mistral") {
        if (getenv_str("MISTRAL_API_KEY").empty()) {
            return "MISTRAL_API_KEY not set";
        }
        return {};
    }
    if (provider == "neutts") {
        if (!path_has_executable("neutts")) {
            return "neutts binary not found on PATH — install with "
                   "'pip install neutts' or 'uv tool install neutts'";
        }
        return {};
    }
    return "Unknown TTS provider: " + provider;
}

// ===========================================================================
// Provider handlers
// ===========================================================================
namespace {

hermes::llm::HttpTransport* g_tts_transport = nullptr;

hermes::llm::HttpTransport* resolve_transport() {
    if (g_tts_transport) return g_tts_transport;
    return hermes::llm::get_default_transport();
}

// -- Edge TTS ---------------------------------------------------------------
std::string provider_edge(const std::string& text, const std::string& voice,
                          const std::string& output_path) {
    std::ostringstream cmd;
    cmd << "edge-tts"
        << " --text " << shell_escape(text)
        << " --voice " << shell_escape(voice)
        << " -w " << shell_escape(output_path);
    nlohmann::json r;
    r["audio_path"] = output_path;
    r["format"] = "mp3";
    r["provider"] = "edge";
    r["voice"] = voice;
    r["command"] = cmd.str();
    return tool_result(r);
}

// -- OpenAI -----------------------------------------------------------------
std::string provider_openai(const std::string& text, const std::string& voice,
                            const std::string& output_path,
                            const nlohmann::json& opts) {
    auto reason = tts_provider_unavailable_reason("openai");
    if (!reason.empty()) return tool_error(reason);
    auto* transport = resolve_transport();
    if (!transport) return tool_error("HTTP transport not available");

    std::string api_key = getenv_str("OPENAI_API_KEY");
    if (api_key.empty()) api_key = getenv_str("VOICE_TOOLS_OPENAI_KEY");

    std::string model = opts.value("model", std::string(kTtsDefaultOpenaiModel));
    std::string openai_voice =
        voice.find("Neural") != std::string::npos ? kTtsDefaultOpenaiVoice
                                                   : voice;
    std::string base_url =
        opts.value("base_url", std::string(kTtsDefaultOpenaiBaseUrl));
    std::string rsp_format = openai_response_format_for(output_path);

    nlohmann::json body{
        {"model", model},
        {"input", text},
        {"voice", openai_voice},
        {"response_format", rsp_format},
    };
    if (opts.contains("speed")) body["speed"] = opts["speed"];
    if (opts.contains("instructions")) body["instructions"] = opts["instructions"];

    std::unordered_map<std::string, std::string> headers{
        {"Content-Type", "application/json"},
        {"Authorization", "Bearer " + api_key},
    };

    auto resp =
        transport->post_json(base_url + "/audio/speech", headers, body.dump());
    if (resp.status_code != 200) {
        return tool_error("OpenAI TTS API error",
                          {{"status", resp.status_code}, {"body", resp.body}});
    }
    write_bytes(output_path, resp.body);
    nlohmann::json r{
        {"audio_path", output_path},
        {"format", rsp_format},
        {"provider", "openai"},
        {"voice", openai_voice},
        {"model", model},
    };
    return tool_result(r);
}

// -- ElevenLabs -------------------------------------------------------------
std::string provider_elevenlabs(const std::string& text,
                                const std::string& voice,
                                const std::string& output_path,
                                const nlohmann::json& opts) {
    auto reason = tts_provider_unavailable_reason("elevenlabs");
    if (!reason.empty()) return tool_error(reason);
    auto* transport = resolve_transport();
    if (!transport) return tool_error("HTTP transport not available");

    std::string api_key = getenv_str("ELEVENLABS_API_KEY");
    std::string voice_id = voice;
    if (voice_id.empty() ||
        voice_id.find("Neural") != std::string::npos ||
        voice_id.find('-') != std::string::npos) {
        voice_id = kTtsDefaultElevenlabsVoiceId;
    }
    std::string model_id =
        opts.value("model_id", std::string(kTtsDefaultElevenlabsModelId));
    std::string out_fmt = elevenlabs_output_format_for(output_path);

    nlohmann::json body{
        {"text", text},
        {"model_id", model_id},
        {"output_format", out_fmt},
    };
    if (opts.contains("voice_settings")) {
        body["voice_settings"] = opts["voice_settings"];
    }

    std::unordered_map<std::string, std::string> headers{
        {"Content-Type", "application/json"},
        {"xi-api-key", api_key},
    };
    auto url =
        std::string("https://api.elevenlabs.io/v1/text-to-speech/") + voice_id;
    auto resp = transport->post_json(url, headers, body.dump());
    if (resp.status_code != 200) {
        return tool_error(
            "ElevenLabs TTS API error",
            {{"status", resp.status_code}, {"body", resp.body}});
    }
    write_bytes(output_path, resp.body);
    nlohmann::json r{
        {"audio_path", output_path},
        {"format", out_fmt},
        {"provider", "elevenlabs"},
        {"voice_id", voice_id},
        {"model_id", model_id},
    };
    return tool_result(r);
}

// -- MiniMax ----------------------------------------------------------------
std::string provider_minimax(const std::string& text,
                             const std::string& voice,
                             const std::string& output_path,
                             const nlohmann::json& opts) {
    auto reason = tts_provider_unavailable_reason("minimax");
    if (!reason.empty()) return tool_error(reason);
    auto* transport = resolve_transport();
    if (!transport) return tool_error("HTTP transport not available");

    std::string api_key = getenv_str("MINIMAX_API_KEY");
    std::string voice_id = voice.empty() ? kTtsDefaultMinimaxVoiceId : voice;
    std::string model =
        opts.value("model", std::string(kTtsDefaultMinimaxModel));
    std::string base_url =
        opts.value("base_url", std::string(kTtsDefaultMinimaxBaseUrl));
    double speed = opts.value("speed", 1.0);
    double vol = opts.value("vol", 1.0);
    double pitch = opts.value("pitch", 0.0);
    std::string audio_format = minimax_audio_format_for(output_path);

    nlohmann::json body{
        {"model", model},
        {"text", text},
        {"stream", false},
        {"voice_setting", {
            {"voice_id", voice_id},
            {"speed", speed},
            {"vol", vol},
            {"pitch", pitch},
        }},
        {"audio_setting", {
            {"sample_rate", 32000},
            {"bitrate", 128000},
            {"format", audio_format},
            {"channel", 1},
        }},
    };

    std::unordered_map<std::string, std::string> headers{
        {"Content-Type", "application/json"},
        {"Authorization", "Bearer " + api_key},
    };
    auto resp = transport->post_json(base_url, headers, body.dump());
    if (resp.status_code != 200) {
        return tool_error("MiniMax TTS API error",
                          {{"status", resp.status_code}, {"body", resp.body}});
    }
    nlohmann::json parsed;
    try {
        parsed = nlohmann::json::parse(resp.body);
    } catch (...) {
        return tool_error("MiniMax TTS returned non-JSON body");
    }
    auto base_resp = parsed.value("base_resp", nlohmann::json::object());
    int status_code = base_resp.value("status_code", -1);
    if (status_code != 0) {
        return tool_error(
            "MiniMax TTS API error (code " + std::to_string(status_code) +
            "): " + base_resp.value("status_msg", std::string("unknown")));
    }
    auto hex_audio =
        parsed.value("data", nlohmann::json::object()).value("audio", "");
    if (hex_audio.empty()) {
        return tool_error("MiniMax TTS returned empty audio data");
    }
    auto decoded = decode_hex(hex_audio);
    if (!decoded) return tool_error("MiniMax TTS returned invalid hex audio");
    write_bytes(output_path, *decoded);

    nlohmann::json r{
        {"audio_path", output_path},
        {"format", audio_format},
        {"provider", "minimax"},
        {"voice_id", voice_id},
        {"model", model},
    };
    return tool_result(r);
}

// -- Mistral Voxtral --------------------------------------------------------
std::string provider_mistral(const std::string& text,
                             const std::string& voice,
                             const std::string& output_path,
                             const nlohmann::json& opts) {
    auto reason = tts_provider_unavailable_reason("mistral");
    if (!reason.empty()) return tool_error(reason);
    auto* transport = resolve_transport();
    if (!transport) return tool_error("HTTP transport not available");

    std::string api_key = getenv_str("MISTRAL_API_KEY");
    std::string voice_id = voice.empty() ? kTtsDefaultMistralVoiceId : voice;
    std::string model =
        opts.value("model", std::string(kTtsDefaultMistralModel));
    std::string rsp_format = mistral_response_format_for(output_path);

    nlohmann::json body{
        {"model", model},
        {"input", text},
        {"voice_id", voice_id},
        {"response_format", rsp_format},
    };
    std::unordered_map<std::string, std::string> headers{
        {"Content-Type", "application/json"},
        {"Authorization", "Bearer " + api_key},
    };
    auto resp = transport->post_json(
        "https://api.mistral.ai/v1/audio/speech/complete", headers,
        body.dump());
    if (resp.status_code != 200) {
        return tool_error("Mistral TTS API error",
                          {{"status", resp.status_code}, {"body", resp.body}});
    }
    nlohmann::json parsed;
    try {
        parsed = nlohmann::json::parse(resp.body);
    } catch (...) {
        return tool_error("Mistral TTS returned non-JSON body");
    }
    auto b64 = parsed.value("audio_data", "");
    if (b64.empty()) return tool_error("Mistral TTS returned empty audio_data");
    auto decoded = decode_base64(b64);
    if (!decoded) return tool_error("Mistral TTS returned invalid base64");
    write_bytes(output_path, *decoded);

    nlohmann::json r{
        {"audio_path", output_path},
        {"format", rsp_format},
        {"provider", "mistral"},
        {"voice_id", voice_id},
        {"model", model},
    };
    return tool_result(r);
}

// -- NeuTTS -----------------------------------------------------------------
std::string provider_neutts(const std::string& text, const std::string& voice,
                            const std::string& output_path) {
    auto reason = tts_provider_unavailable_reason("neutts");
    if (!reason.empty()) return tool_error(reason);

    // NeuTTS outputs WAV natively; if the caller wants .mp3/.ogg we write
    // to .wav first and rename afterwards.
    std::string wav_path = output_path;
    auto ext = ends_with_ext(output_path);
    if (ext != ".wav") {
        auto dot = output_path.rfind('.');
        wav_path = (dot == std::string::npos ? output_path
                                              : output_path.substr(0, dot)) +
                   ".wav";
    }
    std::ostringstream cmd;
    cmd << "neutts --text " << shell_escape(text)
        << " --voice " << shell_escape(voice.empty() ? "default" : voice)
        << " --output " << shell_escape(wav_path) << " 2>&1";
    int rc = std::system(cmd.str().c_str());
    if (rc != 0) {
        return tool_error("neutts failed",
                          {{"exit_code", rc}, {"command", cmd.str()}});
    }

    nlohmann::json r{
        {"audio_path", wav_path},
        {"format", "wav"},
        {"provider", "neutts"},
        {"voice", voice.empty() ? "default" : voice},
    };
    if (wav_path != output_path && has_ffmpeg()) {
        std::ostringstream fc;
        fc << "ffmpeg -i " << shell_escape(wav_path) << " -y -loglevel error "
           << shell_escape(output_path) << " 2>/dev/null";
        int rc2 = std::system(fc.str().c_str());
        if (rc2 == 0) {
            std::error_code ec;
            std::filesystem::remove(wav_path, ec);
            r["audio_path"] = output_path;
            r["format"] = ends_with_ext(output_path).substr(1);
        }
    }
    return tool_result(r);
}

}  // namespace

// ===========================================================================
// Top-level tool handler
// ===========================================================================
namespace {

std::string build_output_path(const ToolContext& ctx,
                              const std::string& provider,
                              const std::string& override_path,
                              bool want_opus) {
    if (!override_path.empty()) return override_path;
    auto tmpdir = std::filesystem::temp_directory_path();
    std::string stem =
        "hermes_tts_" +
        (ctx.task_id.empty() ? std::string("out") : ctx.task_id);
    std::string ext = ".mp3";
    if (want_opus &&
        (provider == "openai" || provider == "elevenlabs" ||
         provider == "mistral")) {
        ext = ".ogg";
    } else if (provider == "neutts") {
        ext = ".wav";
    }
    return (tmpdir / (stem + ext)).string();
}

std::string handle_tts(const nlohmann::json& args, const ToolContext& ctx) {
    if (!args.contains("text") ||
        !args.at("text").is_string() ||
        args.at("text").get<std::string>().empty()) {
        return tool_error("Text is required");
    }
    auto text = args.at("text").get<std::string>();
    text = cap_text_length(text);
    if (args.value("strip_markdown", false)) {
        text = strip_markdown_for_tts(text);
    }

    std::string provider = args.value("provider", std::string(kTtsDefaultProvider));
    std::string voice = args.value("voice", std::string(kTtsDefaultEdgeVoice));
    std::string override_path = args.value("output_path", std::string());
    bool want_opus = args.value("opus", false);

    std::string output_path =
        build_output_path(ctx, provider, override_path, want_opus);

    nlohmann::json opts = args.value("options", nlohmann::json::object());

    if (provider == "edge") {
        return provider_edge(text, voice, output_path);
    }
    if (provider == "openai") {
        return provider_openai(text, voice, output_path, opts);
    }
    if (provider == "elevenlabs") {
        return provider_elevenlabs(text, voice, output_path, opts);
    }
    if (provider == "minimax") {
        return provider_minimax(text, voice, output_path, opts);
    }
    if (provider == "mistral") {
        return provider_mistral(text, voice, output_path, opts);
    }
    if (provider == "neutts") {
        return provider_neutts(text, voice, output_path);
    }
    return tool_error("Unknown TTS provider: " + provider);
}

std::string handle_tts_list_voices(const nlohmann::json& args,
                                   const ToolContext& /*ctx*/) {
    std::string provider =
        args.value("provider", std::string(kTtsDefaultProvider));
    const std::vector<TtsVoice>* src = nullptr;
    if (provider == "edge") src = &edge_voices();
    else if (provider == "openai") src = &openai_voices();
    else if (provider == "elevenlabs") src = &elevenlabs_default_voices();
    else return tool_error("Unknown TTS provider: " + provider);
    nlohmann::json arr = nlohmann::json::array();
    for (const auto& v : *src) {
        arr.push_back({
            {"id", v.id},
            {"name", v.name},
            {"language", v.language},
            {"gender", v.gender},
        });
    }
    nlohmann::json r{
        {"provider", provider},
        {"voices", arr},
        {"count", arr.size()},
    };
    return tool_result(r);
}

}  // namespace

// ---------------------------------------------------------------------------
// Registration
// ---------------------------------------------------------------------------

void register_tts_tools_with_transport(hermes::llm::HttpTransport* transport) {
    g_tts_transport = transport;
    register_tts_tools();
}

void register_tts_tools() {
    auto& reg = ToolRegistry::instance();

    // text_to_speech
    {
        ToolEntry e;
        e.name = "text_to_speech";
        e.toolset = "tts";
        e.description = "Convert text to speech audio";
        e.emoji = "\xF0\x9F\x94\x8A";
        e.schema = {
            {"type", "object"},
            {"properties", {
                {"text", {{"type", "string"},
                          {"description", "Text to synthesize"}}},
                {"voice", {{"type", "string"},
                           {"description", "Voice id/name (optional)"}}},
                {"provider", {{"type", "string"},
                              {"enum", nlohmann::json::array(
                                           {"edge", "elevenlabs", "openai",
                                            "minimax", "mistral", "neutts"})},
                              {"description", "TTS provider (default edge)"}}},
                {"output_path", {{"type", "string"},
                                 {"description", "Absolute output path"}}},
                {"opus", {{"type", "boolean"},
                          {"description",
                           "Prefer OGG/Opus for Telegram voice bubbles"}}},
                {"strip_markdown",
                 {{"type", "boolean"},
                  {"description",
                   "Strip markdown from the text before synthesis"}}},
                {"options", {{"type", "object"},
                             {"description",
                              "Provider-specific options "
                              "(model/base_url/speed/pitch/voice_settings)"}}},
            }},
            {"required", nlohmann::json::array({"text"})}};
        e.handler = handle_tts;
        reg.register_tool(std::move(e));
    }

    // tts_list_voices
    {
        ToolEntry e;
        e.name = "tts_list_voices";
        e.toolset = "tts";
        e.description = "List available TTS voices for a provider";
        e.emoji = "\xF0\x9F\x97\xA3";  // speaking head
        e.schema = {
            {"type", "object"},
            {"properties", {
                {"provider", {{"type", "string"},
                              {"enum", nlohmann::json::array(
                                           {"edge", "openai", "elevenlabs"})},
                              {"description", "Provider (default edge)"}}},
            }},
            {"required", nlohmann::json::array()}};
        e.handler = handle_tts_list_voices;
        reg.register_tool(std::move(e));
    }
}

}  // namespace hermes::tools
