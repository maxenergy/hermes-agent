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
        set_transcription_backend_for_testing({});
        register_transcription_tools();
    }
    void TearDown() override {
        ToolRegistry::instance().clear();
        set_transcription_backend_for_testing({});
    }

    std::filesystem::path make_tmp_wav(const std::string& name = "hermes_tx.wav") {
        auto p = std::filesystem::temp_directory_path() / name;
        std::ofstream ofs(p, std::ios::binary);
        ofs << "RIFF fake wav data";
        return p;
    }
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

TEST_F(TranscriptionToolTest, MissingAudioPathFieldErrors) {
    auto result = ToolRegistry::instance().dispatch(
        "transcribe_audio", nlohmann::json::object(), {});
    auto parsed = nlohmann::json::parse(result);
    EXPECT_TRUE(parsed.contains("error"));
    EXPECT_NE(parsed["error"].get<std::string>().find("audio_path"),
              std::string::npos);
}

TEST_F(TranscriptionToolTest, MockBackendReturnsText) {
    auto tmp = make_tmp_wav("hermes_tx_mock.wav");
    set_transcription_backend_for_testing(
        [](const TranscriptionRequest& req) {
            TranscriptionResult r;
            r.ok = true;
            r.text = "hello world";
            r.language = req.language;
            r.backend = "mock";
            return r;
        });

    auto result = ToolRegistry::instance().dispatch(
        "transcribe_audio",
        {{"audio_path", tmp.string()}, {"language", "en"}},
        {});
    auto parsed = nlohmann::json::parse(result);
    ASSERT_FALSE(parsed.contains("error")) << parsed.dump();
    EXPECT_EQ(parsed["text"], "hello world");
    EXPECT_EQ(parsed["language"], "en");
    EXPECT_EQ(parsed["backend"], "mock");
    EXPECT_EQ(parsed["model"], "base");  // default
    std::filesystem::remove(tmp);
}

TEST_F(TranscriptionToolTest, MockBackendPropagatesModelSize) {
    auto tmp = make_tmp_wav("hermes_tx_model.wav");
    std::string captured_model, captured_lang;
    set_transcription_backend_for_testing(
        [&](const TranscriptionRequest& req) {
            captured_model = req.model;
            captured_lang = req.language;
            TranscriptionResult r;
            r.ok = true;
            r.text = "ok";
            return r;
        });

    auto result = ToolRegistry::instance().dispatch(
        "transcribe_audio",
        {{"audio_path", tmp.string()},
         {"model_size", "small"},
         {"language", "zh"}},
        {});
    auto parsed = nlohmann::json::parse(result);
    ASSERT_FALSE(parsed.contains("error")) << parsed.dump();
    EXPECT_EQ(captured_model, "small");
    EXPECT_EQ(captured_lang, "zh");
    EXPECT_EQ(parsed["model"], "small");
    std::filesystem::remove(tmp);
}

TEST_F(TranscriptionToolTest, MockBackendErrorIsSurfaced) {
    auto tmp = make_tmp_wav("hermes_tx_err.wav");
    set_transcription_backend_for_testing(
        [](const TranscriptionRequest&) {
            TranscriptionResult r;
            r.ok = false;
            r.error = "simulated backend failure";
            return r;
        });

    auto result = ToolRegistry::instance().dispatch(
        "transcribe_audio",
        {{"audio_path", tmp.string()}},
        {});
    auto parsed = nlohmann::json::parse(result);
    ASSERT_TRUE(parsed.contains("error"));
    EXPECT_NE(parsed["error"].get<std::string>().find("simulated"),
              std::string::npos);
    std::filesystem::remove(tmp);
}

TEST_F(TranscriptionToolTest, MockBackendSegmentsArePassedThrough) {
    auto tmp = make_tmp_wav("hermes_tx_seg.wav");
    set_transcription_backend_for_testing(
        [](const TranscriptionRequest&) {
            TranscriptionResult r;
            r.ok = true;
            r.text = "one two";
            r.segments = nlohmann::json::array(
                {{{"start", 0.0}, {"end", 1.0}, {"text", "one"}},
                 {{"start", 1.0}, {"end", 2.0}, {"text", "two"}}});
            return r;
        });

    auto result = ToolRegistry::instance().dispatch(
        "transcribe_audio",
        {{"audio_path", tmp.string()}},
        {});
    auto parsed = nlohmann::json::parse(result);
    ASSERT_FALSE(parsed.contains("error")) << parsed.dump();
    ASSERT_TRUE(parsed.contains("segments"));
    EXPECT_EQ(parsed["segments"].size(), 2u);
    std::filesystem::remove(tmp);
}

// NOTE: End-to-end integration with a real whisper.cpp binary + model
// file is out of scope for CI — it requires a ~150 MB ggml-base.bin
// download.  Run manually with $WHISPER_MODEL_PATH pointing at a model
// dir to smoke-test the subprocess backend.

}  // namespace
