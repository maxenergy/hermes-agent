#include "hermes/tools/registry.hpp"
#include "hermes/tools/tts_tool.hpp"

#include <gtest/gtest.h>

using namespace hermes::tools;

namespace {

class TtsToolTest : public ::testing::Test {
protected:
    void SetUp() override {
        ToolRegistry::instance().clear();
        register_tts_tools();
    }
    void TearDown() override { ToolRegistry::instance().clear(); }
};

TEST_F(TtsToolTest, EdgeProviderConstructsCommand) {
    auto result = ToolRegistry::instance().dispatch(
        "text_to_speech",
        {{"text", "Hello world"}, {"provider", "edge"}},
        {});
    auto parsed = nlohmann::json::parse(result);
    ASSERT_TRUE(parsed.contains("command"));
    auto cmd = parsed["command"].get<std::string>();
    EXPECT_NE(cmd.find("edge-tts"), std::string::npos);
    EXPECT_NE(cmd.find("Hello world"), std::string::npos);
    EXPECT_EQ(parsed["format"], "mp3");
}

TEST_F(TtsToolTest, OpenAiProviderMissingKeyReturnsError) {
    unsetenv("OPENAI_API_KEY");
    auto result = ToolRegistry::instance().dispatch(
        "text_to_speech",
        {{"text", "hi"}, {"provider", "openai"}},
        {});
    auto parsed = nlohmann::json::parse(result);
    EXPECT_TRUE(parsed.contains("error"));
    EXPECT_NE(parsed["error"].get<std::string>().find("OPENAI_API_KEY"),
              std::string::npos);
}

TEST_F(TtsToolTest, ElevenLabsProviderMissingKeyReturnsError) {
    unsetenv("ELEVENLABS_API_KEY");
    auto result = ToolRegistry::instance().dispatch(
        "text_to_speech",
        {{"text", "hi"}, {"provider", "elevenlabs"}},
        {});
    auto parsed = nlohmann::json::parse(result);
    EXPECT_TRUE(parsed.contains("error"));
    EXPECT_NE(parsed["error"].get<std::string>().find("ELEVENLABS_API_KEY"),
              std::string::npos);
}

TEST_F(TtsToolTest, NeuttsMissingBinaryReturnsError) {
    // Guard PATH so neutts can't be found.
    std::string old_path = std::getenv("PATH") ? std::getenv("PATH") : "";
    setenv("PATH", "/nonexistent-path-xyz", 1);
    auto result = ToolRegistry::instance().dispatch(
        "text_to_speech",
        {{"text", "hello"}, {"provider", "neutts"}},
        {});
    auto parsed = nlohmann::json::parse(result);
    EXPECT_TRUE(parsed.contains("error"));
    EXPECT_NE(parsed["error"].get<std::string>().find("neutts"),
              std::string::npos);
    setenv("PATH", old_path.c_str(), 1);
}

TEST_F(TtsToolTest, UnknownProviderReturnsError) {
    auto result = ToolRegistry::instance().dispatch(
        "text_to_speech",
        {{"text", "hi"}, {"provider", "foobar"}},
        {});
    auto parsed = nlohmann::json::parse(result);
    EXPECT_TRUE(parsed.contains("error"));
    EXPECT_NE(parsed["error"].get<std::string>().find("foobar"),
              std::string::npos);
}

}  // namespace
