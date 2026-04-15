// Tests for hermes::cli::models_helpers.
#include "hermes/cli/models_helpers.hpp"

#include <gtest/gtest.h>

using namespace hermes::cli::models_helpers;
using nlohmann::json;

// ---------------------------------------------------------------------------
// Snapshot.
// ---------------------------------------------------------------------------

TEST(ModelsHelpers_Snapshot, NonEmpty) {
    EXPECT_FALSE(openrouter_snapshot().empty());
}

TEST(ModelsHelpers_Snapshot, RecommendedRowFirst) {
    const auto& snap = openrouter_snapshot();
    EXPECT_EQ(snap.front().first, "anthropic/claude-opus-4.6");
    EXPECT_EQ(snap.front().second, "recommended");
}

TEST(ModelsHelpers_Snapshot, ContainsKnownFreeRow) {
    const auto& snap = openrouter_snapshot();
    bool found_free = false;
    for (const auto& kv : snap) {
        if (kv.second == "free") {
            found_free = true;
            break;
        }
    }
    EXPECT_TRUE(found_free);
}

// ---------------------------------------------------------------------------
// Vendor prefix.
// ---------------------------------------------------------------------------

TEST(ModelsHelpers_Vendor, StripsPrefix) {
    EXPECT_EQ(strip_vendor_prefix("anthropic/claude-opus-4.6"),
              "claude-opus-4.6");
    EXPECT_EQ(strip_vendor_prefix("openai/gpt-5.4"), "gpt-5.4");
}

TEST(ModelsHelpers_Vendor, NoPrefixLowercased) {
    EXPECT_EQ(strip_vendor_prefix("GPT-5.4"), "gpt-5.4");
}

TEST(ModelsHelpers_Vendor, EmptyInput) {
    EXPECT_EQ(strip_vendor_prefix(""), "");
}

// ---------------------------------------------------------------------------
// Fast mode.
// ---------------------------------------------------------------------------

TEST(ModelsHelpers_FastMode, OpenAIPriorityModelDetected) {
    EXPECT_TRUE(model_supports_fast_mode("gpt-5.4"));
    EXPECT_TRUE(model_supports_fast_mode("openai/gpt-5.4"));
    EXPECT_TRUE(model_supports_fast_mode("o3"));
}

TEST(ModelsHelpers_FastMode, AnthropicFastModeModelDetected) {
    EXPECT_TRUE(model_supports_fast_mode("claude-opus-4-6"));
    EXPECT_TRUE(model_supports_fast_mode("anthropic/claude-opus-4.6"));
    EXPECT_TRUE(model_supports_fast_mode("anthropic/claude-opus-4.6:beta"));
}

TEST(ModelsHelpers_FastMode, UnsupportedModelRejected) {
    EXPECT_FALSE(model_supports_fast_mode("claude-sonnet-4.5"));
    EXPECT_FALSE(model_supports_fast_mode("kimi-k2.5"));
    EXPECT_FALSE(model_supports_fast_mode(""));
}

TEST(ModelsHelpers_FastMode, AnthropicVsOpenAIClassifier) {
    EXPECT_TRUE(is_anthropic_fast_model("claude-opus-4-6"));
    EXPECT_TRUE(is_anthropic_fast_model("anthropic/claude-opus-4.6"));
    EXPECT_FALSE(is_anthropic_fast_model("gpt-5.4"));
}

TEST(ModelsHelpers_FastMode, OverridesAnthropic) {
    auto ov = resolve_fast_mode_overrides("anthropic/claude-opus-4.6");
    ASSERT_TRUE(ov.has_value());
    EXPECT_EQ((*ov)["speed"], "fast");
}

TEST(ModelsHelpers_FastMode, OverridesOpenAI) {
    auto ov = resolve_fast_mode_overrides("gpt-5.4");
    ASSERT_TRUE(ov.has_value());
    EXPECT_EQ((*ov)["service_tier"], "priority");
}

TEST(ModelsHelpers_FastMode, OverridesUnsupportedNullopt) {
    EXPECT_FALSE(resolve_fast_mode_overrides("claude-haiku-4.5").has_value());
}

// ---------------------------------------------------------------------------
// Free model detection.
// ---------------------------------------------------------------------------

TEST(ModelsHelpers_Free, NonObjectFalse) {
    EXPECT_FALSE(openrouter_model_is_free(json(nullptr)));
    EXPECT_FALSE(openrouter_model_is_free(json("0")));
    EXPECT_FALSE(openrouter_model_is_free(json::array()));
}

TEST(ModelsHelpers_Free, ZeroPricingTrue) {
    json p = {{"prompt", "0"}, {"completion", "0"}};
    EXPECT_TRUE(openrouter_model_is_free(p));
}

TEST(ModelsHelpers_Free, NumericZeroTrue) {
    json p = {{"prompt", 0}, {"completion", 0.0}};
    EXPECT_TRUE(openrouter_model_is_free(p));
}

TEST(ModelsHelpers_Free, NonZeroPricingFalse) {
    json p = {{"prompt", "0.0001"}, {"completion", "0"}};
    EXPECT_FALSE(openrouter_model_is_free(p));
}

TEST(ModelsHelpers_Free, IsModelFreeMissingFalse) {
    json m = {{"openai/gpt-5.4", {{"prompt", "0.0001"}, {"completion", "0"}}}};
    EXPECT_FALSE(is_model_free("missing", m));
}

TEST(ModelsHelpers_Free, IsModelFreeBothZero) {
    json m = {{"a/b", {{"prompt", "0"}, {"completion", "0"}}}};
    EXPECT_TRUE(is_model_free("a/b", m));
}

TEST(ModelsHelpers_Free, IsModelFreeOneNonZero) {
    json m = {{"a/b", {{"prompt", "0"}, {"completion", "0.0002"}}}};
    EXPECT_FALSE(is_model_free("a/b", m));
}

// ---------------------------------------------------------------------------
// Nous tier.
// ---------------------------------------------------------------------------

TEST(ModelsHelpers_NousTier, IsFreeTier) {
    json acct = {{"subscription", {{"monthly_charge", 0}}}};
    EXPECT_TRUE(is_nous_free_tier(acct));
}

TEST(ModelsHelpers_NousTier, PaidTier) {
    json acct = {{"subscription", {{"monthly_charge", 20}}}};
    EXPECT_FALSE(is_nous_free_tier(acct));
}

TEST(ModelsHelpers_NousTier, MissingChargeAssumePaid) {
    json acct = {{"subscription", json::object()}};
    EXPECT_FALSE(is_nous_free_tier(acct));
}

TEST(ModelsHelpers_NousTier, MissingSubAssumePaid) {
    json acct = json::object();
    EXPECT_FALSE(is_nous_free_tier(acct));
}

TEST(ModelsHelpers_NousTier, NumericStringCharge) {
    json acct = {{"subscription", {{"monthly_charge", "0"}}}};
    EXPECT_TRUE(is_nous_free_tier(acct));
}

TEST(ModelsHelpers_FilterFree, NoPricingPassthrough) {
    std::vector<std::string> ids = {"a", "b"};
    auto out = filter_nous_free_models(ids, json::object());
    EXPECT_EQ(out, ids);
}

TEST(ModelsHelpers_FilterFree, PaidKeptAllowlistedFreeKept) {
    std::vector<std::string> ids = {
        "openai/gpt-5.4",        // paid → keep
        "xiaomi/mimo-v2-pro",    // allowlisted + free → keep
    };
    json prices = {
        {"openai/gpt-5.4", {{"prompt", "0.001"}, {"completion", "0.002"}}},
        {"xiaomi/mimo-v2-pro", {{"prompt", "0"}, {"completion", "0"}}},
    };
    auto out = filter_nous_free_models(ids, prices);
    EXPECT_EQ(out, ids);
}

TEST(ModelsHelpers_FilterFree, NonAllowlistFreeDropped) {
    std::vector<std::string> ids = {"random/free-model"};
    json prices = {
        {"random/free-model", {{"prompt", "0"}, {"completion", "0"}}},
    };
    auto out = filter_nous_free_models(ids, prices);
    EXPECT_TRUE(out.empty());
}

TEST(ModelsHelpers_FilterFree, AllowlistPaidDropped) {
    std::vector<std::string> ids = {"xiaomi/mimo-v2-pro"};
    json prices = {
        {"xiaomi/mimo-v2-pro", {{"prompt", "0.001"}, {"completion", "0.002"}}},
    };
    auto out = filter_nous_free_models(ids, prices);
    EXPECT_TRUE(out.empty());
}

TEST(ModelsHelpers_Partition, FreeTierSplit) {
    std::vector<std::string> ids = {"a", "b"};
    json prices = {
        {"a", {{"prompt", "0"}, {"completion", "0"}}},
        {"b", {{"prompt", "0.001"}, {"completion", "0.001"}}},
    };
    auto pr = partition_nous_models_by_tier(ids, prices, true);
    EXPECT_EQ(pr.first, std::vector<std::string>{"a"});
    EXPECT_EQ(pr.second, std::vector<std::string>{"b"});
}

TEST(ModelsHelpers_Partition, PaidTierAllSelectable) {
    std::vector<std::string> ids = {"a", "b"};
    json prices = json::object();
    auto pr = partition_nous_models_by_tier(ids, prices, false);
    EXPECT_EQ(pr.first, ids);
    EXPECT_TRUE(pr.second.empty());
}

// ---------------------------------------------------------------------------
// Pricing formatter.
// ---------------------------------------------------------------------------

TEST(ModelsHelpers_PriceFmt, ZeroIsFree) {
    EXPECT_EQ(format_price_per_mtok("0"), "free");
}

TEST(ModelsHelpers_PriceFmt, BadInputIsQuestion) {
    EXPECT_EQ(format_price_per_mtok(""), "?");
    EXPECT_EQ(format_price_per_mtok("not-a-number"), "?");
}

TEST(ModelsHelpers_PriceFmt, ScalingTo$3PerMtok) {
    EXPECT_EQ(format_price_per_mtok("0.000003"), "$3.00");
}

TEST(ModelsHelpers_PriceFmt, ScalingTo15Cents) {
    EXPECT_EQ(format_price_per_mtok("0.00000015"), "$0.15");
}

TEST(ModelsHelpers_PriceFmt, LargeValue) {
    EXPECT_EQ(format_price_per_mtok("0.00018"), "$180.00");
}

// ---------------------------------------------------------------------------
// JSON normaliser.
// ---------------------------------------------------------------------------

TEST(ModelsHelpers_Payload, BareArray) {
    json p = json::array({json::object({{"id", "a"}}), json("not-object")});
    auto v = payload_items(p);
    ASSERT_EQ(v.size(), 1u);
    EXPECT_EQ(v[0]["id"], "a");
}

TEST(ModelsHelpers_Payload, DataField) {
    json p = {{"data", json::array({json::object({{"id", "a"}}),
                                    json::object({{"id", "b"}})})}};
    auto v = payload_items(p);
    EXPECT_EQ(v.size(), 2u);
}

TEST(ModelsHelpers_Payload, EmptyOnUnknownShape) {
    EXPECT_TRUE(payload_items(json(42)).empty());
    EXPECT_TRUE(payload_items(json("string")).empty());
    EXPECT_TRUE(payload_items(json::object()).empty());
}

// ---------------------------------------------------------------------------
// OpenRouter slug lookup.
// ---------------------------------------------------------------------------

TEST(ModelsHelpers_Slug, ExactMatch) {
    auto s = find_openrouter_slug("anthropic/claude-opus-4.6");
    ASSERT_TRUE(s.has_value());
    EXPECT_EQ(*s, "anthropic/claude-opus-4.6");
}

TEST(ModelsHelpers_Slug, BareNameMatchesAfterSlash) {
    auto s = find_openrouter_slug("claude-opus-4.6");
    ASSERT_TRUE(s.has_value());
    EXPECT_EQ(*s, "anthropic/claude-opus-4.6");
}

TEST(ModelsHelpers_Slug, CaseInsensitive) {
    auto s = find_openrouter_slug("ANTHROPIC/CLAUDE-OPUS-4.6");
    ASSERT_TRUE(s.has_value());
}

TEST(ModelsHelpers_Slug, NotFound) {
    EXPECT_FALSE(find_openrouter_slug("does-not-exist").has_value());
    EXPECT_FALSE(find_openrouter_slug("").has_value());
    EXPECT_FALSE(find_openrouter_slug("   ").has_value());
}

TEST(ModelsHelpers_Slug, CustomCatalog) {
    std::vector<std::string> cat = {"vendor/foo", "vendor/bar"};
    auto s = find_openrouter_slug("foo", cat);
    ASSERT_TRUE(s.has_value());
    EXPECT_EQ(*s, "vendor/foo");
}
