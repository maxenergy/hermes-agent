#include "hermes/tools/registry.hpp"
#include "hermes/tools/tts_tool.hpp"

#include <gtest/gtest.h>

#include <cstdlib>
#include <string>

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

TEST_F(TtsToolTest, EdgeProviderEscapesSingleQuotes) {
    auto result = ToolRegistry::instance().dispatch(
        "text_to_speech",
        {{"text", "it's fine"}, {"provider", "edge"}},
        {});
    auto parsed = nlohmann::json::parse(result);
    ASSERT_TRUE(parsed.contains("command"));
    auto cmd = parsed["command"].get<std::string>();
    // Expect shell-escape pattern: close-quote, escaped quote, reopen.
    EXPECT_NE(cmd.find("'\\''"), std::string::npos);
}

TEST_F(TtsToolTest, EdgeProviderRespectsCustomVoice) {
    auto result = ToolRegistry::instance().dispatch(
        "text_to_speech",
        {{"text", "hi"},
         {"provider", "edge"},
         {"voice", "en-GB-SoniaNeural"}},
        {});
    auto parsed = nlohmann::json::parse(result);
    ASSERT_TRUE(parsed.contains("command"));
    EXPECT_NE(parsed["command"].get<std::string>().find("en-GB-SoniaNeural"),
              std::string::npos);
}

TEST_F(TtsToolTest, OpenAiProviderMissingKeyReturnsError) {
    unsetenv("OPENAI_API_KEY");
    unsetenv("VOICE_TOOLS_OPENAI_KEY");
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

TEST_F(TtsToolTest, MinimaxProviderMissingKeyReturnsError) {
    unsetenv("MINIMAX_API_KEY");
    auto result = ToolRegistry::instance().dispatch(
        "text_to_speech",
        {{"text", "hi"}, {"provider", "minimax"}},
        {});
    auto parsed = nlohmann::json::parse(result);
    EXPECT_TRUE(parsed.contains("error"));
    EXPECT_NE(parsed["error"].get<std::string>().find("MINIMAX_API_KEY"),
              std::string::npos);
}

TEST_F(TtsToolTest, MistralProviderMissingKeyReturnsError) {
    unsetenv("MISTRAL_API_KEY");
    auto result = ToolRegistry::instance().dispatch(
        "text_to_speech",
        {{"text", "hi"}, {"provider", "mistral"}},
        {});
    auto parsed = nlohmann::json::parse(result);
    EXPECT_TRUE(parsed.contains("error"));
    EXPECT_NE(parsed["error"].get<std::string>().find("MISTRAL_API_KEY"),
              std::string::npos);
}

TEST_F(TtsToolTest, NeuttsMissingBinaryReturnsError) {
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

TEST_F(TtsToolTest, EmptyTextReturnsError) {
    auto result = ToolRegistry::instance().dispatch(
        "text_to_speech",
        {{"text", ""}},
        {});
    auto parsed = nlohmann::json::parse(result);
    EXPECT_TRUE(parsed.contains("error"));
}

TEST_F(TtsToolTest, ListVoicesEdgeReturnsNonEmpty) {
    auto result = ToolRegistry::instance().dispatch(
        "tts_list_voices",
        {{"provider", "edge"}},
        {});
    auto parsed = nlohmann::json::parse(result);
    ASSERT_TRUE(parsed.contains("voices"));
    EXPECT_GT(parsed["voices"].size(), 0u);
    EXPECT_EQ(parsed["provider"], "edge");
}

TEST_F(TtsToolTest, ListVoicesOpenAiReturnsNonEmpty) {
    auto result = ToolRegistry::instance().dispatch(
        "tts_list_voices",
        {{"provider", "openai"}},
        {});
    auto parsed = nlohmann::json::parse(result);
    ASSERT_TRUE(parsed.contains("voices"));
    EXPECT_GT(parsed["voices"].size(), 0u);
}

TEST_F(TtsToolTest, ListVoicesElevenLabsReturnsNonEmpty) {
    auto result = ToolRegistry::instance().dispatch(
        "tts_list_voices",
        {{"provider", "elevenlabs"}},
        {});
    auto parsed = nlohmann::json::parse(result);
    ASSERT_TRUE(parsed.contains("voices"));
    EXPECT_GT(parsed["voices"].size(), 0u);
}

TEST_F(TtsToolTest, ListVoicesUnknownProviderReturnsError) {
    auto result = ToolRegistry::instance().dispatch(
        "tts_list_voices",
        {{"provider", "doesnotexist"}},
        {});
    auto parsed = nlohmann::json::parse(result);
    EXPECT_TRUE(parsed.contains("error"));
}

TEST_F(TtsToolTest, SchemaExposesAllProviders) {
    auto defs = ToolRegistry::instance().get_definitions();
    bool found = false;
    for (const auto& d : defs) {
        if (d.name != "text_to_speech") continue;
        found = true;
        const auto& schema = d.parameters;
        ASSERT_TRUE(schema.contains("properties"));
        const auto& enums = schema["properties"]["provider"]["enum"];
        EXPECT_EQ(enums.size(), 6u);
    }
    EXPECT_TRUE(found);
}

TEST_F(TtsToolTest, RegisterBothTools) {
    auto defs = ToolRegistry::instance().get_definitions();
    bool has_tts = false, has_voices = false;
    for (const auto& d : defs) {
        if (d.name == "text_to_speech") has_tts = true;
        if (d.name == "tts_list_voices") has_voices = true;
    }
    EXPECT_TRUE(has_tts);
    EXPECT_TRUE(has_voices);
}

}  // namespace
