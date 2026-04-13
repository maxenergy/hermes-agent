#include "hermes/tools/ffmpeg.hpp"
#include "hermes/tools/registry.hpp"
#include "hermes/tools/transcription_tool.hpp"
#include "hermes/tools/voice_mode_tool.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>

using namespace hermes::tools;

namespace {

// Test-only recorder.  start() writes a tiny fake WAV to the returned
// path; stop() returns the same path.  `is_recording()` flips true
// between start/stop without any real audio hardware.
class MockRecorder : public AudioRecorder {
public:
    std::string start(std::string& error) override {
        if (recording_) {
            error = "already recording";
            return {};
        }
        path_ = (std::filesystem::temp_directory_path() /
                 "hermes_voice_mock.wav").string();
        std::ofstream ofs(path_, std::ios::binary);
        ofs << "RIFF mock wav";
        recording_ = true;
        start_ = std::chrono::steady_clock::now();
        return path_;
    }
    std::string stop() override {
        if (!recording_) return path_;
        recording_ = false;
        return path_;
    }
    void cancel() override {
        recording_ = false;
        std::error_code ec;
        std::filesystem::remove(path_, ec);
        path_.clear();
    }
    bool is_recording() const override { return recording_; }
    std::chrono::milliseconds elapsed() const override {
        if (!recording_) return std::chrono::milliseconds{0};
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start_);
    }
    std::string backend_name() const override { return "mock"; }

private:
    bool recording_ = false;
    std::string path_;
    std::chrono::steady_clock::time_point start_;
};

class VoiceModeToolTest : public ::testing::Test {
protected:
    void SetUp() override {
        ToolRegistry::instance().clear();
        VoiceSession::instance().reset();
        set_recorder_factory_for_testing(
            [] { return std::make_unique<MockRecorder>(); });
        set_transcription_backend_for_testing(
            [](const TranscriptionRequest& req) {
                TranscriptionResult r;
                r.ok = true;
                r.text = "hello from mock";
                r.language = req.language.empty() ? "en" : req.language;
                r.backend = "mock";
                return r;
            });
        register_transcription_tools();
        register_voice_tools();
    }
    void TearDown() override {
        ToolRegistry::instance().clear();
        VoiceSession::instance().reset();
        set_recorder_factory_for_testing({});
        set_transcription_backend_for_testing({});
    }
};

TEST_F(VoiceModeToolTest, StartSetsListening) {
    auto result = ToolRegistry::instance().dispatch(
        "voice_mode", {{"action", "start"}}, {});
    auto parsed = nlohmann::json::parse(result);
    EXPECT_TRUE(parsed.value("started", false));
    EXPECT_EQ(parsed["state"], "listening");
    EXPECT_EQ(VoiceSession::instance().state(), VoiceState::Listening);
}

TEST_F(VoiceModeToolTest, StopSetsInactive) {
    ToolRegistry::instance().dispatch(
        "voice_mode", {{"action", "start"}}, {});
    auto result = ToolRegistry::instance().dispatch(
        "voice_mode", {{"action", "stop"}}, {});
    auto parsed = nlohmann::json::parse(result);
    EXPECT_TRUE(parsed.value("stopped", false));
    EXPECT_EQ(VoiceSession::instance().state(), VoiceState::Inactive);
}

TEST_F(VoiceModeToolTest, StatusReflectsState) {
    auto result = ToolRegistry::instance().dispatch(
        "voice_mode", {{"action", "status"}}, {});
    auto parsed = nlohmann::json::parse(result);
    EXPECT_EQ(parsed["state"], "inactive");

    ToolRegistry::instance().dispatch(
        "voice_mode",
        {{"action", "start"}, {"config", {{"tts_voice", "alloy"}}}},
        {});
    result = ToolRegistry::instance().dispatch(
        "voice_mode", {{"action", "status"}}, {});
    parsed = nlohmann::json::parse(result);
    EXPECT_EQ(parsed["state"], "listening");
    EXPECT_EQ(parsed["config"]["tts_voice"], "alloy");
    EXPECT_FALSE(parsed.value("recording", true));
}

TEST_F(VoiceModeToolTest, DoubleStartIdempotent) {
    ToolRegistry::instance().dispatch(
        "voice_mode", {{"action", "start"}}, {});
    auto result = ToolRegistry::instance().dispatch(
        "voice_mode", {{"action", "start"}}, {});
    auto parsed = nlohmann::json::parse(result);
    EXPECT_TRUE(parsed.value("started", false));
    EXPECT_EQ(parsed["state"], "listening");
}

TEST_F(VoiceModeToolTest, RecordRequiresActiveSession) {
    auto result = ToolRegistry::instance().dispatch(
        "voice_mode", {{"action", "record"}}, {});
    auto parsed = nlohmann::json::parse(result);
    ASSERT_TRUE(parsed.contains("error"));
    EXPECT_NE(parsed["error"].get<std::string>().find("inactive"),
              std::string::npos);
}

TEST_F(VoiceModeToolTest, RecordStartsCapture) {
    ToolRegistry::instance().dispatch(
        "voice_mode", {{"action", "start"}}, {});
    auto result = ToolRegistry::instance().dispatch(
        "voice_mode", {{"action", "record"}}, {});
    auto parsed = nlohmann::json::parse(result);
    ASSERT_FALSE(parsed.contains("error")) << parsed.dump();
    EXPECT_TRUE(parsed.value("recording", false));
    EXPECT_FALSE(parsed.value("path", std::string{}).empty());

    auto status = nlohmann::json::parse(
        ToolRegistry::instance().dispatch(
            "voice_mode", {{"action", "status"}}, {}));
    EXPECT_TRUE(status.value("recording", false));
    EXPECT_EQ(status.value("backend", std::string{}), "mock");
}

TEST_F(VoiceModeToolTest, TranscribeEndToEnd) {
    ToolRegistry::instance().dispatch(
        "voice_mode", {{"action", "start"}}, {});
    ToolRegistry::instance().dispatch(
        "voice_mode", {{"action", "record"}}, {});
    auto result = ToolRegistry::instance().dispatch(
        "voice_mode",
        {{"action", "transcribe"},
         {"language", "en"},
         {"model_size", "small"}},
        {});
    auto parsed = nlohmann::json::parse(result);
    ASSERT_FALSE(parsed.contains("error")) << parsed.dump();
    EXPECT_EQ(parsed["text"], "hello from mock");
    ASSERT_TRUE(parsed.contains("events"));
    ASSERT_EQ(parsed["events"].size(), 1u);
    EXPECT_EQ(parsed["events"][0]["kind"], "final");
    EXPECT_EQ(parsed["events"][0]["text"], "hello from mock");

    // After transcription the session returns to listening.
    EXPECT_EQ(VoiceSession::instance().state(), VoiceState::Listening);
}

TEST_F(VoiceModeToolTest, TranscribeWithoutRecordingErrors) {
    ToolRegistry::instance().dispatch(
        "voice_mode", {{"action", "start"}}, {});
    auto result = ToolRegistry::instance().dispatch(
        "voice_mode", {{"action", "transcribe"}}, {});
    auto parsed = nlohmann::json::parse(result);
    ASSERT_TRUE(parsed.contains("error"));
    EXPECT_NE(parsed["error"].get<std::string>().find("no recording"),
              std::string::npos);
}

TEST_F(VoiceModeToolTest, TranscribeSurfacesBackendErrorAsEvent) {
    set_transcription_backend_for_testing(
        [](const TranscriptionRequest&) {
            TranscriptionResult r;
            r.ok = false;
            r.error = "mock-whisper-offline";
            return r;
        });

    ToolRegistry::instance().dispatch(
        "voice_mode", {{"action", "start"}}, {});
    ToolRegistry::instance().dispatch(
        "voice_mode", {{"action", "record"}}, {});
    auto result = ToolRegistry::instance().dispatch(
        "voice_mode", {{"action", "transcribe"}}, {});
    auto parsed = nlohmann::json::parse(result);
    ASSERT_TRUE(parsed.contains("error"));
    EXPECT_NE(parsed["error"].get<std::string>().find("mock-whisper-offline"),
              std::string::npos);

    auto events_json = nlohmann::json::parse(
        ToolRegistry::instance().dispatch(
            "voice_mode", {{"action", "events"}}, {}));
    ASSERT_TRUE(events_json.contains("events"));
    ASSERT_EQ(events_json["events"].size(), 1u);
    EXPECT_EQ(events_json["events"][0]["kind"], "error");
}

TEST_F(VoiceModeToolTest, UnknownActionErrors) {
    auto result = ToolRegistry::instance().dispatch(
        "voice_mode", {{"action", "fly_to_moon"}}, {});
    auto parsed = nlohmann::json::parse(result);
    ASSERT_TRUE(parsed.contains("error"));
    EXPECT_NE(parsed["error"].get<std::string>().find("unknown"),
              std::string::npos);
}

// --- Streaming pipeline tests ------------------------------------------

class VoiceStreamingTest : public ::testing::Test {
protected:
    void SetUp() override {
        ToolRegistry::instance().clear();
        VoiceSession::instance().reset();
        set_recorder_factory_for_testing(
            [] { return std::make_unique<MockRecorder>(); });
        // Provide a fake transcription backend that echoes a static text
        // for each segment request, with a counter suffix so we can tell
        // segments apart.
        segment_count_ = 0;
        set_transcription_backend_for_testing(
            [this](const TranscriptionRequest& req) {
                TranscriptionResult r;
                r.ok = true;
                r.text = "segment" + std::to_string(++segment_count_);
                r.language = req.language.empty() ? "en" : req.language;
                r.backend = "mock";
                return r;
            });
        // Fake ffmpeg: silencedetect returns a single silence between
        // 1.0s and 1.6s; the trim slices succeed silently.
        hermes::tools::ffmpeg::set_ffmpeg_binary_for_testing(
            "/usr/bin/ffmpeg-fake");
        hermes::tools::ffmpeg::set_ffprobe_binary_for_testing(
            "/usr/bin/ffprobe-fake");
        hermes::tools::ffmpeg::set_command_runner_for_testing(
            [](const hermes::tools::ffmpeg::CommandInvocation& inv)
                -> hermes::tools::ffmpeg::CommandOutcome {
                hermes::tools::ffmpeg::CommandOutcome o;
                o.exit_code = 0;
                // silencedetect invocation has the `silencedetect=` arg.
                for (const auto& a : inv.argv) {
                    if (a.find("silencedetect=") != std::string::npos) {
                        o.stderr_text =
                            "[silencedetect @ 0x1] silence_start: 1.0\n"
                            "[silencedetect @ 0x1] silence_end: 1.6 | "
                            "silence_duration: 0.6\n";
                        return o;
                    }
                }
                // Slice commands — touch the output file so the caller
                // sees it exists.
                if (!inv.argv.empty()) {
                    const auto& out_path = inv.argv.back();
                    std::ofstream ofs(out_path, std::ios::binary);
                    ofs << "RIFFfake";
                }
                return o;
            });

        set_voice_responder_for_testing(
            [this](const std::string& transcript, const std::string&) {
                responder_calls_.push_back(transcript);
                return "reply:" + transcript;
            });
        set_voice_speaker_for_testing(
            [this](const std::string& text, const std::string&,
                   std::string&) {
                speaker_calls_.push_back(text);
                return "/tmp/hermes_tts_out_" +
                       std::to_string(speaker_calls_.size()) + ".mp3";
            });
        set_voice_playback_for_testing(
            [this](const std::string& path) {
                playback_calls_.push_back(path);
                return true;
            });
        register_transcription_tools();
        register_voice_tools();
    }
    void TearDown() override {
        ToolRegistry::instance().clear();
        VoiceSession::instance().reset();
        set_recorder_factory_for_testing({});
        set_transcription_backend_for_testing({});
        set_voice_responder_for_testing({});
        set_voice_speaker_for_testing({});
        set_voice_playback_for_testing({});
        hermes::tools::ffmpeg::set_ffmpeg_binary_for_testing({});
        hermes::tools::ffmpeg::set_ffprobe_binary_for_testing({});
        hermes::tools::ffmpeg::set_command_runner_for_testing({});
    }

    int segment_count_ = 0;
    std::vector<std::string> responder_calls_;
    std::vector<std::string> speaker_calls_;
    std::vector<std::string> playback_calls_;
};

TEST_F(VoiceStreamingTest, RequiresAudioPath) {
    auto result = ToolRegistry::instance().dispatch(
        "voice_mode", {{"action", "streaming"}}, {});
    auto parsed = nlohmann::json::parse(result);
    ASSERT_TRUE(parsed.contains("error"));
    EXPECT_NE(parsed["error"].get<std::string>().find("requires audio_path"),
              std::string::npos);
}

TEST_F(VoiceStreamingTest, RunsFullPipelineOverTwoSegments) {
    StreamingPipelineConfig cfg;
    cfg.audio_path = "/tmp/hermes_test_input.wav";
    cfg.speak_response = true;
    cfg.playback_response = true;
    auto res = run_streaming_pipeline(cfg);
    EXPECT_TRUE(res.ok);

    // One silence splits two segments → 2 transcriptions, 2 responses,
    // 2 TTS, 2 playbacks, plus partials + final + done events.
    int finals = 0, responses = 0, speaking = 0, done = 0;
    for (const auto& ev : res.events) {
        if (ev.kind == "final") ++finals;
        else if (ev.kind == "response") ++responses;
        else if (ev.kind == "speaking") ++speaking;
        else if (ev.kind == "done") ++done;
    }
    EXPECT_EQ(finals, 2);
    EXPECT_EQ(responses, 2);
    EXPECT_EQ(speaking, 2);
    EXPECT_EQ(done, 1);
    EXPECT_EQ(responder_calls_.size(), 2u);
    EXPECT_EQ(responder_calls_[0], "segment1");
    EXPECT_EQ(responder_calls_[1], "segment2");
    EXPECT_EQ(speaker_calls_.size(), 2u);
    EXPECT_EQ(speaker_calls_[0], "reply:segment1");
    EXPECT_EQ(playback_calls_.size(), 2u);
}

TEST_F(VoiceStreamingTest, SkipsResponseWhenNoResponder) {
    set_voice_responder_for_testing({});
    StreamingPipelineConfig cfg;
    cfg.audio_path = "/tmp/hermes_test_input.wav";
    auto res = run_streaming_pipeline(cfg);
    EXPECT_TRUE(res.ok);
    int finals = 0, responses = 0;
    for (const auto& ev : res.events) {
        if (ev.kind == "final") ++finals;
        if (ev.kind == "response") ++responses;
    }
    EXPECT_EQ(finals, 2);
    EXPECT_EQ(responses, 0);
    EXPECT_TRUE(speaker_calls_.empty());
}

TEST_F(VoiceStreamingTest, SkipsPlaybackWhenFlagOff) {
    StreamingPipelineConfig cfg;
    cfg.audio_path = "/tmp/hermes_test_input.wav";
    cfg.speak_response = true;
    cfg.playback_response = false;
    auto res = run_streaming_pipeline(cfg);
    EXPECT_TRUE(res.ok);
    EXPECT_FALSE(speaker_calls_.empty());
    EXPECT_TRUE(playback_calls_.empty());
}

TEST_F(VoiceStreamingTest, ToolActionDispatches) {
    auto result = ToolRegistry::instance().dispatch(
        "voice_mode",
        {{"action", "streaming"},
         {"audio_path", "/tmp/hermes_test_input.wav"},
         {"speak_response", true},
         {"playback_response", false},
         {"language", "en"}},
        {});
    auto parsed = nlohmann::json::parse(result);
    ASSERT_TRUE(parsed.value("ok", false)) << parsed.dump();
    ASSERT_TRUE(parsed.contains("events"));
    auto events = parsed["events"];
    bool saw_done = false;
    for (const auto& ev : events) {
        if (ev.value("kind", std::string{}) == "done") saw_done = true;
    }
    EXPECT_TRUE(saw_done);
}

TEST_F(VoiceModeToolTest, StopWhileRecordingCleansUp) {
    ToolRegistry::instance().dispatch(
        "voice_mode", {{"action", "start"}}, {});
    ToolRegistry::instance().dispatch(
        "voice_mode", {{"action", "record"}}, {});
    ASSERT_TRUE(VoiceSession::instance().status().value("recording", false));

    ToolRegistry::instance().dispatch(
        "voice_mode", {{"action", "stop"}}, {});
    EXPECT_EQ(VoiceSession::instance().state(), VoiceState::Inactive);
    EXPECT_FALSE(VoiceSession::instance().status().value("recording", true));
}

}  // namespace
