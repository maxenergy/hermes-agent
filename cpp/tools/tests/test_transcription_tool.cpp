#include "hermes/tools/registry.hpp"
#include "hermes/tools/transcription_tool.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>

using namespace hermes::tools;

namespace {

class TranscriptionToolTest : public ::testing::Test {
protected:
    void SetUp() override {
        ToolRegistry::instance().clear();
        register_transcription_tools();
    }
    void TearDown() override { ToolRegistry::instance().clear(); }
};

TEST_F(TranscriptionToolTest, MissingFileReturnsError) {
    auto result = ToolRegistry::instance().dispatch(
        "transcribe_audio",
        {{"audio_path", "/nonexistent/path/audio.mp3"}},
        {});
    auto parsed = nlohmann::json::parse(result);
    EXPECT_TRUE(parsed.contains("error"));
    EXPECT_NE(parsed["error"].get<std::string>().find("not found"),
              std::string::npos);
}

TEST_F(TranscriptionToolTest, WrongExtensionReturnsError) {
    // Create a temporary file with a non-audio extension.
    auto tmp = std::filesystem::temp_directory_path() / "hermes_test_bad.txt";
    { std::ofstream ofs(tmp); ofs << "not audio"; }
    auto result = ToolRegistry::instance().dispatch(
        "transcribe_audio",
        {{"audio_path", tmp.string()}},
        {});
    auto parsed = nlohmann::json::parse(result);
    EXPECT_TRUE(parsed.contains("error"));
    EXPECT_NE(parsed["error"].get<std::string>().find("unsupported"),
              std::string::npos);
    std::filesystem::remove(tmp);
}

TEST_F(TranscriptionToolTest, ValidAudioReturnsBackendNotAvailable) {
    // Create a temporary .wav file.
    auto tmp = std::filesystem::temp_directory_path() / "hermes_test.wav";
    { std::ofstream ofs(tmp); ofs << "RIFF fake wav data"; }
    auto result = ToolRegistry::instance().dispatch(
        "transcribe_audio",
        {{"audio_path", tmp.string()}},
        {});
    auto parsed = nlohmann::json::parse(result);
    EXPECT_TRUE(parsed.contains("error"));
    EXPECT_NE(parsed["error"].get<std::string>().find("faster-whisper"),
              std::string::npos);
    std::filesystem::remove(tmp);
}

}  // namespace
