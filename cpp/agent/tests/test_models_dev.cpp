// Unit tests for hermes::agent::models_dev (C++17 port of
// agent/models_dev.py).  Uses set_injected_catalog() to avoid touching
// the real models.dev service.
#include "hermes/agent/models_dev.hpp"

#include <gtest/gtest.h>

#include <optional>
#include <string>

using namespace hermes::agent::models_dev;

namespace {

const char* kSampleCatalog = R"({
  "anthropic": {
    "name": "Anthropic",
    "api": "https://api.anthropic.com",
    "doc": "https://docs.anthropic.com",
    "env": ["ANTHROPIC_API_KEY"],
    "models": {
      "claude-opus-4": {
        "name": "Claude Opus 4",
        "family": "claude",
        "reasoning": true,
        "tool_call": true,
        "attachment": true,
        "structured_output": false,
        "limit": {"context": 200000, "output": 8192, "input": 200000},
        "cost": {"input": 15.0, "output": 75.0, "cache_read": 1.5},
        "modalities": {"input": ["text", "image"], "output": ["text"]},
        "knowledge": "2025-03",
        "release_date": "2025-05-01",
        "status": ""
      },
      "claude-3-5-haiku": {
        "name": "Claude Haiku 3.5",
        "family": "claude",
        "tool_call": true,
        "limit": {"context": 200000, "output": 8192},
        "cost": {"input": 1.0, "output": 5.0}
      },
      "claude-3-tts": {
        "tool_call": true,
        "limit": {"context": 0}
      }
    }
  },
  "google": {
    "name": "Google",
    "env": ["GEMINI_API_KEY"],
    "models": {
      "gemini-2.5-pro": {
        "family": "gemini",
        "tool_call": true,
        "reasoning": true,
        "attachment": true,
        "limit": {"context": 1000000, "output": 65536},
        "cost": {"input": 1.25, "output": 10.0},
        "modalities": {"input": ["text", "image", "pdf", "audio"]}
      },
      "gemini-embedding-001": {
        "tool_call": false
      }
    }
  }
})";

class ModelsDevTest : public ::testing::Test {
protected:
    void SetUp() override {
        auto parsed = nlohmann::json::parse(kSampleCatalog);
        set_injected_catalog(parsed);
    }
    void TearDown() override {
        set_injected_catalog(std::nullopt);
        reset_memory_cache();
    }
};

TEST_F(ModelsDevTest, ProviderMappingHermesToModelsDev) {
    const auto& m = provider_to_models_dev();
    EXPECT_EQ(m.at("anthropic"), "anthropic");
    EXPECT_EQ(m.at("gemini"), "google");
    EXPECT_EQ(m.at("kilocode"), "kilo");
}

TEST_F(ModelsDevTest, ReverseMappingConsistent) {
    const auto& rev = models_dev_to_provider();
    EXPECT_EQ(rev.at("kilo"), "kilocode");
    EXPECT_TRUE(rev.find("google") != rev.end());
}

TEST_F(ModelsDevTest, FetchReturnsInjectedCatalog) {
    auto data = fetch_models_dev();
    ASSERT_TRUE(data.is_object());
    EXPECT_TRUE(data.contains("anthropic"));
    EXPECT_TRUE(data.contains("google"));
}

TEST_F(ModelsDevTest, LookupContextExactAndCaseInsensitive) {
    EXPECT_EQ(lookup_models_dev_context("anthropic", "claude-opus-4"),
              200000);
    EXPECT_EQ(lookup_models_dev_context("anthropic", "CLAUDE-OPUS-4"),
              200000);
    EXPECT_EQ(lookup_models_dev_context("anthropic", "claude-3-tts"), 0);
    EXPECT_EQ(lookup_models_dev_context("unknown-provider", "x"), 0);
}

TEST_F(ModelsDevTest, ModelCapabilitiesStructured) {
    auto caps = get_model_capabilities("gemini", "gemini-2.5-pro");
    ASSERT_TRUE(caps.has_value());
    EXPECT_TRUE(caps->supports_tools);
    EXPECT_TRUE(caps->supports_vision);
    EXPECT_TRUE(caps->supports_reasoning);
    EXPECT_EQ(caps->context_window, 1000000);
    EXPECT_EQ(caps->max_output_tokens, 65536);
    EXPECT_EQ(caps->model_family, "gemini");
}

TEST_F(ModelsDevTest, ModelCapabilitiesMissingDefaultsApply) {
    auto caps = get_model_capabilities("anthropic", "claude-3-5-haiku");
    ASSERT_TRUE(caps.has_value());
    EXPECT_FALSE(caps->supports_vision);
    EXPECT_FALSE(caps->supports_reasoning);
    // family defaults to "claude".
    EXPECT_EQ(caps->model_family, "claude");
}

TEST_F(ModelsDevTest, ListProviderModels) {
    auto list = list_provider_models("anthropic");
    EXPECT_EQ(list.size(), 3u);
}

TEST_F(ModelsDevTest, NoisePatternDetection) {
    EXPECT_TRUE(is_noise_model_id("text-embedding-3-small"));
    // Note: ``-tts\b`` requires a leading hyphen → ``tts-1`` alone does not match.
    EXPECT_TRUE(is_noise_model_id("gpt-4-tts"));
    EXPECT_TRUE(is_noise_model_id("gpt-4o-mini-tts"));
    EXPECT_TRUE(is_noise_model_id("gemini-2.0-live-001"));
    EXPECT_TRUE(is_noise_model_id("gemini-2.0-exp-1217_preview"));
    EXPECT_FALSE(is_noise_model_id("claude-opus-4"));
    EXPECT_FALSE(is_noise_model_id("gpt-4o"));
}

TEST_F(ModelsDevTest, ListAgenticModelsFiltersNoise) {
    auto list = list_agentic_models("anthropic");
    // claude-3-tts is noise → filtered.  The other two have tool_call=true.
    EXPECT_EQ(list.size(), 2u);
    for (const auto& m : list) {
        EXPECT_TRUE(m.find("tts") == std::string::npos);
    }
}

TEST_F(ModelsDevTest, ListAgenticModelsSkipsNonTool) {
    auto list = list_agentic_models("gemini");
    // gemini-embedding-001 has tool_call=false → excluded.
    EXPECT_EQ(list.size(), 1u);
    EXPECT_EQ(list[0], "gemini-2.5-pro");
}

TEST_F(ModelsDevTest, SearchSubstringMatches) {
    auto results = search_models_dev("opus");
    ASSERT_FALSE(results.empty());
    EXPECT_EQ(results[0].model_id, "claude-opus-4");
    EXPECT_EQ(results[0].provider, "anthropic");
}

TEST_F(ModelsDevTest, SearchRestrictedToProvider) {
    auto results = search_models_dev("gemini", "gemini");
    ASSERT_FALSE(results.empty());
    for (const auto& r : results) {
        EXPECT_EQ(r.provider, "gemini");
    }
}

TEST_F(ModelsDevTest, SearchLimitHonored) {
    auto results = search_models_dev("claude", "anthropic", 2);
    EXPECT_LE(results.size(), 2u);
}

TEST_F(ModelsDevTest, ParseModelInfoRichFields) {
    auto mi = get_model_info("anthropic", "claude-opus-4");
    ASSERT_TRUE(mi.has_value());
    EXPECT_EQ(mi->id, "claude-opus-4");
    EXPECT_EQ(mi->family, "claude");
    EXPECT_TRUE(mi->tool_call);
    EXPECT_TRUE(mi->reasoning);
    EXPECT_TRUE(mi->attachment);
    EXPECT_EQ(mi->context_window, 200000);
    EXPECT_EQ(mi->max_output, 8192);
    ASSERT_TRUE(mi->max_input.has_value());
    EXPECT_EQ(*mi->max_input, 200000);
    EXPECT_DOUBLE_EQ(mi->cost_input, 15.0);
    EXPECT_DOUBLE_EQ(mi->cost_output, 75.0);
    ASSERT_TRUE(mi->cost_cache_read.has_value());
    EXPECT_DOUBLE_EQ(*mi->cost_cache_read, 1.5);
    EXPECT_EQ(mi->knowledge_cutoff, "2025-03");
}

TEST_F(ModelsDevTest, ModelInfoHelpers) {
    auto mi = get_model_info("gemini", "gemini-2.5-pro");
    ASSERT_TRUE(mi.has_value());
    EXPECT_TRUE(mi->supports_vision());
    EXPECT_TRUE(mi->supports_pdf());
    EXPECT_TRUE(mi->supports_audio_input());
    EXPECT_TRUE(mi->has_cost_data());
    auto caps = mi->format_capabilities();
    EXPECT_TRUE(caps.find("reasoning") != std::string::npos);
    EXPECT_TRUE(caps.find("tools") != std::string::npos);
    EXPECT_TRUE(caps.find("vision") != std::string::npos);
    EXPECT_TRUE(caps.find("PDF") != std::string::npos);
}

TEST_F(ModelsDevTest, FormatCostUnknown) {
    ModelInfo mi;
    EXPECT_EQ(mi.format_cost(), "unknown");
    EXPECT_EQ(mi.format_capabilities(), "basic");
}

TEST_F(ModelsDevTest, FormatCostWithCacheRead) {
    ModelInfo mi;
    mi.cost_input = 3.0;
    mi.cost_output = 15.0;
    mi.cost_cache_read = 0.3;
    auto s = mi.format_cost();
    EXPECT_TRUE(s.find("$3.00/M in") != std::string::npos);
    EXPECT_TRUE(s.find("$15.00/M out") != std::string::npos);
    EXPECT_TRUE(s.find("cache read $0.30/M") != std::string::npos);
}

TEST_F(ModelsDevTest, ProviderInfoParsed) {
    auto pi = get_provider_info("anthropic");
    ASSERT_TRUE(pi.has_value());
    EXPECT_EQ(pi->name, "Anthropic");
    EXPECT_EQ(pi->api, "https://api.anthropic.com");
    EXPECT_EQ(pi->model_count, 3u);
    ASSERT_FALSE(pi->env.empty());
    EXPECT_EQ(pi->env[0], "ANTHROPIC_API_KEY");
}

TEST_F(ModelsDevTest, UnknownProviderReturnsNullopt) {
    EXPECT_FALSE(get_provider_info("not-a-real-provider").has_value());
    EXPECT_FALSE(
        get_model_info("anthropic", "not-a-real-model").has_value());
}

TEST_F(ModelsDevTest, HermesIdResolvesToModelsDevId) {
    // "gemini" Hermes → "google" models.dev.
    auto mi = get_model_info("gemini", "gemini-2.5-pro");
    ASSERT_TRUE(mi.has_value());
    EXPECT_EQ(mi->provider_id, "google");
}

}  // namespace
