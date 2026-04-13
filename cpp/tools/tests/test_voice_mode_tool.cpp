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
