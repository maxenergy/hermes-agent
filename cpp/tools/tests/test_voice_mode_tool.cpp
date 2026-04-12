#include "hermes/tools/registry.hpp"
#include "hermes/tools/voice_mode_tool.hpp"

#include <gtest/gtest.h>

using namespace hermes::tools;

namespace {

class VoiceModeToolTest : public ::testing::Test {
protected:
    void SetUp() override {
        ToolRegistry::instance().clear();
        VoiceSession::instance().reset();
        register_voice_tools();
    }
    void TearDown() override {
        ToolRegistry::instance().clear();
        VoiceSession::instance().reset();
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
    // Start first, then stop.
    ToolRegistry::instance().dispatch(
        "voice_mode", {{"action", "start"}}, {});
    auto result = ToolRegistry::instance().dispatch(
        "voice_mode", {{"action", "stop"}}, {});
    auto parsed = nlohmann::json::parse(result);
    EXPECT_TRUE(parsed.value("stopped", false));
    EXPECT_EQ(VoiceSession::instance().state(), VoiceState::Inactive);
}

TEST_F(VoiceModeToolTest, StatusReflectsState) {
    // Initially inactive.
    auto result = ToolRegistry::instance().dispatch(
        "voice_mode", {{"action", "status"}}, {});
    auto parsed = nlohmann::json::parse(result);
    EXPECT_EQ(parsed["state"], "inactive");

    // After start → listening.
    ToolRegistry::instance().dispatch(
        "voice_mode",
        {{"action", "start"}, {"config", {{"tts_voice", "alloy"}}}},
        {});
    result = ToolRegistry::instance().dispatch(
        "voice_mode", {{"action", "status"}}, {});
    parsed = nlohmann::json::parse(result);
    EXPECT_EQ(parsed["state"], "listening");
    EXPECT_EQ(parsed["config"]["tts_voice"], "alloy");
}

TEST_F(VoiceModeToolTest, DoubleStartIdempotent) {
    ToolRegistry::instance().dispatch(
        "voice_mode", {{"action", "start"}}, {});
    auto result = ToolRegistry::instance().dispatch(
        "voice_mode", {{"action", "start"}}, {});
    auto parsed = nlohmann::json::parse(result);
    EXPECT_TRUE(parsed.value("started", false));
    EXPECT_EQ(parsed["state"], "listening");
    EXPECT_EQ(VoiceSession::instance().state(), VoiceState::Listening);
}

}  // namespace
