// Unit tests for auxiliary_fallback.hpp — provider fallback chain planner.
#include "hermes/llm/auxiliary_fallback.hpp"

#include <gtest/gtest.h>

using namespace hermes::llm;

TEST(AuxiliaryFallback, ProviderNameRoundTrip) {
    for (auto p : {AuxiliaryProvider::OpenRouter,
                   AuxiliaryProvider::NousPortal,
                   AuxiliaryProvider::CodexOauth,
                   AuxiliaryProvider::Anthropic,
                   AuxiliaryProvider::Zai,
                   AuxiliaryProvider::Gemini}) {
        auto name = auxiliary_provider_name(p);
        auto back = auxiliary_provider_from_name(name);
        EXPECT_EQ(back, p) << "name=" << name;
    }
}

TEST(AuxiliaryFallback, AliasResolution) {
    EXPECT_EQ(auxiliary_provider_from_name("claude"), AuxiliaryProvider::Anthropic);
    EXPECT_EQ(auxiliary_provider_from_name("z.ai"),   AuxiliaryProvider::Zai);
    EXPECT_EQ(auxiliary_provider_from_name("glm"),    AuxiliaryProvider::Zai);
    EXPECT_EQ(auxiliary_provider_from_name("moonshot"), AuxiliaryProvider::KimiCoding);
    EXPECT_EQ(auxiliary_provider_from_name("google"), AuxiliaryProvider::Gemini);
    EXPECT_EQ(auxiliary_provider_from_name("codex"),  AuxiliaryProvider::CodexOauth);
    EXPECT_EQ(auxiliary_provider_from_name("bogus"),  AuxiliaryProvider::Unknown);
}

TEST(AuxiliaryFallback, CreditExhaustion402Always) {
    EXPECT_TRUE(is_credit_exhaustion_error(402, ""));
    EXPECT_TRUE(is_credit_exhaustion_error(402, "anything"));
}

TEST(AuxiliaryFallback, CreditExhaustionByKeyword) {
    EXPECT_TRUE(is_credit_exhaustion_error(429, "You are out of credits"));
    EXPECT_TRUE(is_credit_exhaustion_error(429, "insufficient funds for operation"));
    EXPECT_TRUE(is_credit_exhaustion_error(0, "Cannot afford this request"));
    EXPECT_TRUE(is_credit_exhaustion_error(0, "quota exceeded"));
}

TEST(AuxiliaryFallback, CreditExhaustionRejectsUnrelated) {
    EXPECT_FALSE(is_credit_exhaustion_error(500, "internal server error"));
    EXPECT_FALSE(is_credit_exhaustion_error(401, "invalid api key"));
    EXPECT_FALSE(is_credit_exhaustion_error(403, "forbidden"));
    EXPECT_FALSE(is_credit_exhaustion_error(429, "rate limit exceeded"));
}

TEST(AuxiliaryFallback, ConnectionErrorDetection) {
    EXPECT_TRUE(is_connection_error("Connection refused"));
    EXPECT_TRUE(is_connection_error("operation timed out"));
    EXPECT_TRUE(is_connection_error("DNS resolution failed"));
    EXPECT_TRUE(is_connection_error("connection reset by peer"));
    EXPECT_FALSE(is_connection_error("Invalid API key"));
    EXPECT_FALSE(is_connection_error(""));
}

TEST(AuxiliaryFallback, DefaultAuxiliaryModelTextChain) {
    EXPECT_EQ(default_auxiliary_model(AuxiliaryProvider::Zai),
              "glm-4.5-flash");
    EXPECT_EQ(default_auxiliary_model(AuxiliaryProvider::KimiCoding),
              "kimi-k2-turbo-preview");
    EXPECT_EQ(default_auxiliary_model(AuxiliaryProvider::Minimax),
              "MiniMax-M2.7");
    EXPECT_FALSE(default_auxiliary_model(AuxiliaryProvider::Anthropic).empty());
}

TEST(AuxiliaryFallback, DefaultAuxiliaryModelCustomEmpty) {
    EXPECT_TRUE(default_auxiliary_model(AuxiliaryProvider::Custom).empty());
    EXPECT_TRUE(default_auxiliary_model(AuxiliaryProvider::Unknown).empty());
}

TEST(AuxiliaryFallback, BuildFallbackChainTextPlacesMainFirst) {
    auto chain = build_fallback_chain(AuxiliaryProvider::Zai);
    ASSERT_FALSE(chain.empty());
    EXPECT_EQ(chain.front(), AuxiliaryProvider::Zai);
    // OpenRouter, NousPortal, etc. should appear after.
    EXPECT_GE(chain.size(), 5u);
    // No duplicates.
    std::set<int> seen;
    for (auto p : chain) {
        EXPECT_TRUE(seen.insert(static_cast<int>(p)).second)
            << "duplicate provider in chain";
    }
}

TEST(AuxiliaryFallback, BuildFallbackChainUnknownMainSkipped) {
    auto chain = build_fallback_chain(AuxiliaryProvider::Unknown);
    ASSERT_FALSE(chain.empty());
    // First is OpenRouter (top of the auto chain).
    EXPECT_EQ(chain.front(), AuxiliaryProvider::OpenRouter);
}

TEST(AuxiliaryFallback, BuildFallbackChainVision) {
    auto chain = build_fallback_chain(AuxiliaryProvider::OpenRouter, true);
    ASSERT_FALSE(chain.empty());
    EXPECT_EQ(chain.front(), AuxiliaryProvider::OpenRouter);
    // Vision chain excludes zai/kimi/minimax (no vision support there).
    for (auto p : chain) {
        EXPECT_NE(p, AuxiliaryProvider::Zai);
        EXPECT_NE(p, AuxiliaryProvider::KimiCoding);
        EXPECT_NE(p, AuxiliaryProvider::Minimax);
    }
}

TEST(AuxiliaryFallback, RunFallbackStopsOnFirstSuccess) {
    auto chain = build_fallback_chain(AuxiliaryProvider::OpenRouter);
    int calls = 0;
    auto dispatch = [&](AuxiliaryProvider p, std::string_view) {
        ++calls;
        AttemptObservation obs;
        obs.provider = p;
        obs.latency = std::chrono::milliseconds(50);
        if (p == AuxiliaryProvider::OpenRouter) {
            obs.succeeded = true;
        } else {
            obs.http_status = 402;
        }
        return obs;
    };
    auto r = run_fallback_chain(chain, dispatch);
    EXPECT_EQ(calls, 1);
    ASSERT_TRUE(r.winning_attempt.has_value());
    EXPECT_EQ(r.winning_attempt->provider, AuxiliaryProvider::OpenRouter);
}

TEST(AuxiliaryFallback, RunFallbackAdvancesOn402) {
    std::vector<AuxiliaryProvider> chain = {
        AuxiliaryProvider::OpenRouter,
        AuxiliaryProvider::NousPortal,
        AuxiliaryProvider::CodexOauth,
    };
    int calls = 0;
    auto dispatch = [&](AuxiliaryProvider p, std::string_view) {
        ++calls;
        AttemptObservation obs;
        obs.provider = p;
        obs.latency = std::chrono::milliseconds(20 + calls);
        if (p == AuxiliaryProvider::CodexOauth) {
            obs.succeeded = true;
            obs.usage.input_tokens = 100;
            obs.usage.output_tokens = 50;
            obs.estimated_cost_usd = 0.001;
        } else {
            obs.http_status = 402;
            obs.error_message = "out of credits";
            obs.error_kind = "credits";
        }
        return obs;
    };
    auto r = run_fallback_chain(chain, dispatch);
    EXPECT_EQ(calls, 3);
    ASSERT_TRUE(r.winning_attempt.has_value());
    EXPECT_EQ(r.winning_attempt->provider, AuxiliaryProvider::CodexOauth);
    EXPECT_EQ(r.history.size(), 3u);
    EXPECT_NEAR(r.total_cost_usd, 0.001, 1e-12);
}

TEST(AuxiliaryFallback, RunFallbackStopsOnNonTransientError) {
    std::vector<AuxiliaryProvider> chain = {
        AuxiliaryProvider::OpenRouter,
        AuxiliaryProvider::NousPortal,
    };
    int calls = 0;
    auto dispatch = [&](AuxiliaryProvider, std::string_view) {
        ++calls;
        AttemptObservation obs;
        obs.http_status = 400;  // non-transient
        obs.error_message = "bad request";
        return obs;
    };
    auto r = run_fallback_chain(chain, dispatch);
    EXPECT_EQ(calls, 1);
    EXPECT_FALSE(r.winning_attempt.has_value());
}

TEST(AuxiliaryFallback, RunFallbackAdvancesOnConnectionError) {
    std::vector<AuxiliaryProvider> chain = {
        AuxiliaryProvider::OpenRouter,
        AuxiliaryProvider::NousPortal,
    };
    int calls = 0;
    auto dispatch = [&](AuxiliaryProvider p, std::string_view) {
        ++calls;
        AttemptObservation obs;
        obs.provider = p;
        if (p == AuxiliaryProvider::OpenRouter) {
            // No http_status → treated as transport error.
            obs.error_message = "Connection refused";
        } else {
            obs.succeeded = true;
        }
        return obs;
    };
    auto r = run_fallback_chain(chain, dispatch);
    EXPECT_EQ(calls, 2);
    ASSERT_TRUE(r.winning_attempt.has_value());
    EXPECT_EQ(r.winning_attempt->provider, AuxiliaryProvider::NousPortal);
}

TEST(AuxiliaryFallback, RunFallbackEmptyChainNoOp) {
    auto r = run_fallback_chain({}, [](auto, auto) { return AttemptObservation{}; });
    EXPECT_FALSE(r.winning_attempt.has_value());
    EXPECT_TRUE(r.history.empty());
}

TEST(AuxiliaryFallback, RunFallbackAggregatesLatency) {
    std::vector<AuxiliaryProvider> chain = {
        AuxiliaryProvider::OpenRouter,
        AuxiliaryProvider::NousPortal,
        AuxiliaryProvider::CodexOauth,
    };
    auto dispatch = [&](AuxiliaryProvider p, std::string_view) {
        AttemptObservation obs;
        obs.provider = p;
        obs.latency = std::chrono::milliseconds(100);
        if (p == AuxiliaryProvider::CodexOauth) {
            obs.succeeded = true;
        } else {
            obs.http_status = 402;
            obs.error_message = "credits exhausted";
        }
        return obs;
    };
    auto r = run_fallback_chain(chain, dispatch);
    EXPECT_EQ(r.total_latency.count(), 300);
}
