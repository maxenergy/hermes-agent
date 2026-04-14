// Phase 8: Text-to-speech tool — edge-tts CLI + HTTP providers.
//
// The implementation lives in ``cpp/tools/src/tts_tool.cpp`` and supports
// six providers (edge / elevenlabs / openai / minimax / mistral / neutts)
// with shared helpers for markdown stripping, base64/hex decoding, sentence
// splitting, output-format negotiation, and optional ffmpeg Opus conversion.
#pragma once

#include <string>
#include <vector>

namespace hermes::llm {
class HttpTransport;
}  // namespace hermes::llm

namespace hermes::tools {

// ---------------------------------------------------------------------------
// Defaults — mirrors ``tools/tts_tool.py`` module-level constants.
// ---------------------------------------------------------------------------
inline constexpr const char* kTtsDefaultProvider = "edge";
inline constexpr const char* kTtsDefaultEdgeVoice = "en-US-AriaNeural";
inline constexpr const char* kTtsDefaultOpenaiModel = "gpt-4o-mini-tts";
inline constexpr const char* kTtsDefaultOpenaiVoice = "alloy";
inline constexpr const char* kTtsDefaultOpenaiBaseUrl =
    "https://api.openai.com/v1";
inline constexpr const char* kTtsDefaultElevenlabsVoiceId =
    "pNInz6obpgDQGcFmaJgB";
inline constexpr const char* kTtsDefaultElevenlabsModelId =
    "eleven_multilingual_v2";
inline constexpr const char* kTtsDefaultElevenlabsStreamingModelId =
    "eleven_flash_v2_5";
inline constexpr const char* kTtsDefaultMinimaxModel = "speech-2.8-hd";
inline constexpr const char* kTtsDefaultMinimaxVoiceId =
    "English_Graceful_Lady";
inline constexpr const char* kTtsDefaultMinimaxBaseUrl =
    "https://api.minimax.io/v1/t2a_v2";
inline constexpr const char* kTtsDefaultMistralTtsModel =
    "voxtral-mini-tts-2603";
inline constexpr const char* kTtsDefaultMistralVoiceId =
    "c69964a6-ab8b-4f8a-9465-ec0925096ec8";
inline constexpr std::size_t kTtsMaxTextLength = 4000;

// ---------------------------------------------------------------------------
// Voice catalogue entry — used by ``tts_list_voices``.
// ---------------------------------------------------------------------------
struct TtsVoice {
    std::string id;
    std::string name;
    std::string language;
    std::string gender;
};

// Register ``text_to_speech`` and ``tts_list_voices`` with the global tool
// registry using the default HTTP transport.
void register_tts_tools();

// Same as ``register_tts_tools`` but uses an injected transport — intended
// for tests that want to stub out network I/O.  Pass ``nullptr`` to fall
// back to the default transport.
void register_tts_tools_with_transport(hermes::llm::HttpTransport* transport);

}  // namespace hermes::tools
