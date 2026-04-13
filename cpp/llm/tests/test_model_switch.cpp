#include "hermes/llm/model_switch.hpp"

#include "hermes/llm/credential_pool.hpp"
#include "hermes/llm/llm_client.hpp"

#include <gtest/gtest.h>

#include <cstdlib>
#include <memory>
#include <nlohmann/json.hpp>

using namespace hermes::llm;

namespace {

// Minimal stub client for factory wiring tests.
class StubClient : public LlmClient {
public:
    explicit StubClient(std::string name) : name_(std::move(name)) {}
    CompletionResponse complete(const CompletionRequest&) override {
        return CompletionResponse{};
    }
    std::string provider_name() const override { return name_; }

private:
    std::string name_;
};

void scrub_api_keys() {
    for (auto* v : {"OPENAI_API_KEY", "ANTHROPIC_API_KEY", "ANTHROPIC_TOKEN",
                    "OPENROUTER_API_KEY"}) {
        ::unsetenv(v);
    }
}

void prime_pool(CredentialPool& pool) {
    pool.store("anthropic", PooledCredential{"ak", "https://api.anthropic.com",
                                              {}, "oauth"});
    pool.store("openai",
               PooledCredential{"sk", "https://api.openai.com/v1", {}, "env"});
    pool.store("qwen-oauth", PooledCredential{"qk",
                                              "https://dashscope.aliyuncs.com/compatible-mode/v1",
                                              {}, "qwen-oauth"});
}

}  // namespace

TEST(ModelSwitch, BuildsResolvedProvider) {
    scrub_api_keys();
    CredentialPool pool;
    prime_pool(pool);
    nlohmann::json cfg = nlohmann::json::object();
    auto r = switch_model("claude-opus-4-6", cfg, "", &pool);
    EXPECT_EQ(r.resolved.provider_name, "anthropic");
    EXPECT_EQ(r.resolved.model_id, "claude-opus-4-6");
    EXPECT_EQ(r.resolved.api_mode, "anthropic_messages");
    EXPECT_FALSE(r.summary.empty());
}

TEST(ModelSwitch, TokenizerInvalidatedOnFamilyChange) {
    scrub_api_keys();
    CredentialPool pool;
    prime_pool(pool);
    nlohmann::json cfg = nlohmann::json::object();

    // openai → anthropic: tokenizer family changes.
    auto r = switch_model("claude-sonnet-4-6", cfg, "openai", &pool);
    EXPECT_TRUE(r.tokenizer_invalidated);

    // openai → openai-codex: same family (openai).
    auto r2 = switch_model("gpt-5.3-codex", cfg, "openai", &pool);
    EXPECT_FALSE(r2.tokenizer_invalidated);

    // No previous provider → never invalidate.
    auto r3 = switch_model("gpt-4o", cfg, "", &pool);
    EXPECT_FALSE(r3.tokenizer_invalidated);
}

TEST(ModelSwitch, FactoryProducesClient) {
    scrub_api_keys();
    CredentialPool pool;
    prime_pool(pool);

    set_llm_client_factory([](const ResolvedProvider& rp) {
        return std::static_pointer_cast<LlmClient>(
            std::make_shared<StubClient>(rp.provider_name));
    });

    nlohmann::json cfg = nlohmann::json::object();
    auto r = switch_model("gpt-4o", cfg, "", &pool);
    ASSERT_TRUE(r.client != nullptr);
    EXPECT_EQ(r.client->provider_name(), "openai");

    // Reset factory so it doesn't leak to other tests.
    set_llm_client_factory(nullptr);
}

TEST(ModelSwitch, TierDownForAnthropicOpus) {
    auto tiers = build_tier_down("claude-opus-4-6");
    ASSERT_FALSE(tiers.empty());
    EXPECT_EQ(tiers[0], "claude-sonnet-4-6");
}

TEST(ModelSwitch, TierDownForAnthropicSonnet) {
    auto tiers = build_tier_down("claude-sonnet-4-6");
    ASSERT_FALSE(tiers.empty());
    EXPECT_EQ(tiers[0], "claude-haiku-4-6");
}

TEST(ModelSwitch, TierDownForOpenAi) {
    auto tiers = build_tier_down("gpt-5.4");
    EXPECT_FALSE(tiers.empty());
    EXPECT_EQ(tiers[0], "gpt-5.4-mini");
}

TEST(ModelSwitch, TierDownForCodex) {
    auto tiers = build_tier_down("gpt-5.3-codex");
    EXPECT_FALSE(tiers.empty());
    EXPECT_EQ(tiers[0], "gpt-5.1-codex-mini");
}

TEST(ModelSwitch, TierDownForQwenCoder) {
    auto tiers = build_tier_down("qwen3-coder-plus");
    EXPECT_FALSE(tiers.empty());
    EXPECT_EQ(tiers[0], "qwen-turbo");
}

TEST(ModelSwitch, TierDownEmptyForUnknownModel) {
    auto tiers = build_tier_down("mystery");
    EXPECT_TRUE(tiers.empty());
}

TEST(ModelSwitch, EmptyModelThrows) {
    CredentialPool pool;
    nlohmann::json cfg = nlohmann::json::object();
    EXPECT_THROW(switch_model("", cfg, "", &pool), std::invalid_argument);
}

TEST(ModelSwitch, PreservesSessionStateAcrossSwitches) {
    // "Session state preservation" here means the switch doesn't touch
    // any shared global state; subsequent switches produce consistent
    // ResolvedProvider results independent of history.
    scrub_api_keys();
    CredentialPool pool;
    prime_pool(pool);
    nlohmann::json cfg = nlohmann::json::object();

    auto r1 = switch_model("gpt-4o", cfg, "", &pool);
    auto r2 = switch_model("claude-opus-4-6", cfg, r1.resolved.provider_name, &pool);
    auto r3 = switch_model("gpt-4o", cfg, r2.resolved.provider_name, &pool);

    // Switching back to gpt-4o produces the same resolved provider as
    // the original switch.
    EXPECT_EQ(r1.resolved.provider_name, r3.resolved.provider_name);
    EXPECT_EQ(r1.resolved.api_key, r3.resolved.api_key);
    EXPECT_EQ(r1.resolved.base_url, r3.resolved.base_url);
    // And the tokenizer was invalidated on both family transitions.
    EXPECT_TRUE(r2.tokenizer_invalidated);  // openai → anthropic
    EXPECT_TRUE(r3.tokenizer_invalidated);  // anthropic → openai
}
