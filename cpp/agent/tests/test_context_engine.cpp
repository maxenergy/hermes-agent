// Tests for ContextEngine default behavior.
#include "hermes/agent/context_engine.hpp"

#include <gtest/gtest.h>

namespace {

// Minimal concrete subclass for testing default methods.
class TestEngine : public hermes::agent::ContextEngine {
public:
    std::string_view name() const override { return "test"; }
    std::vector<hermes::llm::Message> compress(
        std::vector<hermes::llm::Message> m, int64_t, int64_t) override {
        return m;
    }
    void on_session_reset() override {}
    void update_model(const hermes::llm::ModelMetadata& meta) override {
        context_length = meta.context_length;
        threshold_tokens = static_cast<std::int64_t>(
            context_length * threshold_percent);
    }
};

}  // namespace

TEST(ContextEngine, UpdateFromResponseCanonical) {
    TestEngine e;
    nlohmann::json usage = {
        {"prompt_tokens", 120},
        {"completion_tokens", 30},
        {"total_tokens", 150},
    };
    e.update_from_response(usage);
    EXPECT_EQ(e.last_prompt_tokens, 120);
    EXPECT_EQ(e.last_completion_tokens, 30);
    EXPECT_EQ(e.last_total_tokens, 150);
}

TEST(ContextEngine, UpdateFromResponseAnthropicAliases) {
    TestEngine e;
    nlohmann::json usage = {
        {"input_tokens", 200},
        {"output_tokens", 50},
    };
    e.update_from_response(usage);
    EXPECT_EQ(e.last_prompt_tokens, 200);
    EXPECT_EQ(e.last_completion_tokens, 50);
    EXPECT_EQ(e.last_total_tokens, 250);
}

TEST(ContextEngine, ShouldCompressBelowThreshold) {
    TestEngine e;
    e.threshold_tokens = 100;
    e.last_prompt_tokens = 50;
    EXPECT_FALSE(e.should_compress());
    EXPECT_TRUE(e.should_compress(110));
}

TEST(ContextEngine, ShouldCompressNoThresholdReturnsFalse) {
    TestEngine e;
    EXPECT_FALSE(e.should_compress(9999));
}

TEST(ContextEngine, PreflightDefaultsFalse) {
    TestEngine e;
    std::vector<hermes::llm::Message> msgs;
    EXPECT_FALSE(e.should_compress_preflight(msgs));
}

TEST(ContextEngine, DefaultGetToolSchemasEmpty) {
    TestEngine e;
    EXPECT_TRUE(e.get_tool_schemas().empty());
}

TEST(ContextEngine, HandleToolCallReturnsError) {
    TestEngine e;
    std::string out = e.handle_tool_call("foo", nlohmann::json::object());
    auto j = nlohmann::json::parse(out);
    EXPECT_TRUE(j.contains("error"));
    EXPECT_NE(j["error"].get<std::string>().find("foo"), std::string::npos);
}

TEST(ContextEngine, StatusReportsUsagePercent) {
    TestEngine e;
    e.context_length = 1000;
    e.last_prompt_tokens = 250;
    e.threshold_tokens = 750;
    e.compression_count = 2;
    auto s = e.get_status();
    EXPECT_EQ(s.last_prompt_tokens, 250);
    EXPECT_EQ(s.context_length, 1000);
    EXPECT_EQ(s.threshold_tokens, 750);
    EXPECT_DOUBLE_EQ(s.usage_percent, 25.0);
    EXPECT_EQ(s.compression_count, 2);
}

TEST(ContextEngine, StatusCapsAt100) {
    TestEngine e;
    e.context_length = 100;
    e.last_prompt_tokens = 500;
    auto s = e.get_status();
    EXPECT_DOUBLE_EQ(s.usage_percent, 100.0);
}

TEST(ContextEngine, StatusZeroContextLength) {
    TestEngine e;
    auto s = e.get_status();
    EXPECT_DOUBLE_EQ(s.usage_percent, 0.0);
}

TEST(ContextEngine, UpdateModelSetsThreshold) {
    TestEngine e;
    hermes::llm::ModelMetadata meta;
    meta.context_length = 40000;
    e.update_model(meta);
    EXPECT_EQ(e.context_length, 40000);
    EXPECT_EQ(e.threshold_tokens, 30000);
}

TEST(ContextEngine, StatusToJsonRoundTrip) {
    hermes::agent::ContextEngineStatus s;
    s.last_prompt_tokens = 42;
    s.context_length = 1000;
    s.compression_count = 3;
    auto j = s.to_json();
    EXPECT_EQ(j["last_prompt_tokens"], 42);
    EXPECT_EQ(j["context_length"], 1000);
    EXPECT_EQ(j["compression_count"], 3);
}
