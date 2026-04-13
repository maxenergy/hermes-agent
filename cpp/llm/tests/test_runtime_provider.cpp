#include "hermes/llm/runtime_provider.hpp"

#include "hermes/llm/credential_pool.hpp"

#include <gtest/gtest.h>

#include <cstdlib>
#include <nlohmann/json.hpp>

using hermes::llm::CredentialPool;
using hermes::llm::infer_provider_from_model;
using hermes::llm::PooledCredential;
using hermes::llm::resolve_runtime_provider;

namespace {

class EnvGuard {
public:
    EnvGuard(const char* name, const char* value) : name_(name) {
        const char* prev = std::getenv(name);
        if (prev) prev_ = prev;
        had_prev_ = prev != nullptr;
        if (value) {
            ::setenv(name, value, 1);
        } else {
            ::unsetenv(name);
        }
    }
    ~EnvGuard() {
        if (had_prev_) {
            ::setenv(name_, prev_.c_str(), 1);
        } else {
            ::unsetenv(name_);
        }
    }

private:
    const char* name_;
    std::string prev_;
    bool had_prev_ = false;
};

// Strip common API-key env vars so ambient shell config can't leak.
void scrub_api_keys() {
    for (auto* v : {"OPENAI_API_KEY", "ANTHROPIC_API_KEY", "ANTHROPIC_TOKEN",
                    "OPENROUTER_API_KEY", "HERMES_OPENAI_API_KEY",
                    "QWEN_OAUTH_API_KEY", "HERMES_QWEN_OAUTH_API_KEY",
                    "COPILOT_API_KEY", "NOUS_API_KEY"}) {
        ::unsetenv(v);
    }
}

}  // namespace

TEST(InferProvider, Anthropic) {
    EXPECT_EQ(infer_provider_from_model("claude-sonnet-4-6"), "anthropic");
    EXPECT_EQ(infer_provider_from_model("claude-opus-4-6"), "anthropic");
    EXPECT_EQ(infer_provider_from_model("anthropic/claude-opus-4-6"), "anthropic");
}

TEST(InferProvider, OpenAIAndCodex) {
    EXPECT_EQ(infer_provider_from_model("gpt-4o"), "openai");
    EXPECT_EQ(infer_provider_from_model("gpt-4o-mini"), "openai");
    EXPECT_EQ(infer_provider_from_model("gpt-5.3-codex"), "openai-codex");
    EXPECT_EQ(infer_provider_from_model("gpt-5.1-codex-mini"), "openai-codex");
    EXPECT_EQ(infer_provider_from_model("openai/gpt-4o"), "openai");
    EXPECT_EQ(infer_provider_from_model("openai/gpt-5.3-codex"), "openai-codex");
}

TEST(InferProvider, QwenCopilotNousOthers) {
    EXPECT_EQ(infer_provider_from_model("qwen3-coder-plus"), "qwen-oauth");
    EXPECT_EQ(infer_provider_from_model("copilot/gpt-4o"), "copilot");
    EXPECT_EQ(infer_provider_from_model("nous/hermes-4-405b"), "nous");
    EXPECT_EQ(infer_provider_from_model("gemini-2.5-pro"), "google");
    EXPECT_EQ(infer_provider_from_model("deepseek-chat"), "deepseek");
    EXPECT_EQ(infer_provider_from_model("grok-2"), "x-ai");
    EXPECT_EQ(infer_provider_from_model("openrouter/anthropic/claude-opus"),
              "openrouter");
    // Unknown slug → aggregator fallback.
    EXPECT_EQ(infer_provider_from_model("mystery-model"), "openrouter");
}

TEST(ResolveRuntimeProvider, ConfigProviderOverridesInference) {
    scrub_api_keys();
    CredentialPool pool;
    nlohmann::json cfg = {
        {"model", {{"provider", "custom"}, {"base_url", "https://localhost:8080"},
                   {"api_key", "local-test"}}},
    };
    auto r = resolve_runtime_provider("claude-opus-4-6", cfg, &pool);
    EXPECT_EQ(r.provider_name, "custom");
    EXPECT_EQ(r.api_key, "local-test");
    EXPECT_EQ(r.base_url, "https://localhost:8080");
    EXPECT_EQ(r.api_mode, "chat_completions");
    EXPECT_EQ(r.source, "config");
}

TEST(ResolveRuntimeProvider, AnthropicViaPoolEntry) {
    scrub_api_keys();
    CredentialPool pool;
    PooledCredential c;
    c.api_key = "sk-ant-test";
    c.base_url = "https://api.anthropic.com";
    c.source = "oauth";
    pool.store("anthropic", c);

    nlohmann::json cfg = nlohmann::json::object();
    auto r = resolve_runtime_provider("claude-sonnet-4-6", cfg, &pool);
    EXPECT_EQ(r.provider_name, "anthropic");
    EXPECT_EQ(r.api_key, "sk-ant-test");
    EXPECT_EQ(r.base_url, "https://api.anthropic.com");
    EXPECT_EQ(r.api_mode, "anthropic_messages");
    EXPECT_EQ(r.model_id, "claude-sonnet-4-6");
    EXPECT_EQ(r.source, "oauth");
}

TEST(ResolveRuntimeProvider, OpenAIViaEnvVar) {
    scrub_api_keys();
    EnvGuard g("OPENAI_API_KEY", "sk-env");
    CredentialPool pool;
    nlohmann::json cfg = nlohmann::json::object();
    auto r = resolve_runtime_provider("gpt-4o", cfg, &pool);
    EXPECT_EQ(r.provider_name, "openai");
    EXPECT_EQ(r.api_key, "sk-env");
    EXPECT_EQ(r.api_mode, "chat_completions");
    EXPECT_EQ(r.source, "env");
    EXPECT_FALSE(r.base_url.empty());
}

TEST(ResolveRuntimeProvider, QwenOauthViaPoolRefresher) {
    scrub_api_keys();
    CredentialPool pool;
    pool.set_refresher("qwen-oauth", [](const std::string&) {
        PooledCredential c;
        c.api_key = "qwen-fresh";
        c.base_url = "https://dashscope.aliyuncs.com/compatible-mode/v1";
        c.expires_at = std::chrono::system_clock::now() + std::chrono::minutes(30);
        c.source = "qwen-oauth";
        return c;
    });
    nlohmann::json cfg = nlohmann::json::object();
    auto r = resolve_runtime_provider("qwen3-coder-plus", cfg, &pool);
    EXPECT_EQ(r.provider_name, "qwen-oauth");
    EXPECT_EQ(r.api_key, "qwen-fresh");
    EXPECT_EQ(r.source, "qwen-oauth");
}

TEST(ResolveRuntimeProvider, CopilotViaPool) {
    scrub_api_keys();
    CredentialPool pool;
    PooledCredential c;
    c.api_key = "ghu_test";
    c.base_url = "https://api.githubcopilot.com";
    c.source = "oauth";
    pool.store("copilot", c);
    nlohmann::json cfg = {{"model", {{"provider", "copilot"}}}};
    auto r = resolve_runtime_provider("gpt-4o", cfg, &pool);
    EXPECT_EQ(r.provider_name, "copilot");
    EXPECT_EQ(r.api_key, "ghu_test");
    EXPECT_EQ(r.api_mode, "chat_completions");
}

TEST(ResolveRuntimeProvider, NousViaPool) {
    scrub_api_keys();
    CredentialPool pool;
    PooledCredential c;
    c.api_key = "nous-agent-key";
    c.base_url = "https://inference-api.nousresearch.com/v1";
    c.source = "portal";
    pool.store("nous", c);
    nlohmann::json cfg = nlohmann::json::object();
    auto r = resolve_runtime_provider("nous/hermes-4-405b", cfg, &pool);
    EXPECT_EQ(r.provider_name, "nous");
    EXPECT_EQ(r.api_key, "nous-agent-key");
}

TEST(ResolveRuntimeProvider, CodexResponsesApiMode) {
    scrub_api_keys();
    CredentialPool pool;
    pool.store("openai-codex", PooledCredential{
        /*api_key=*/"ey_codex", "https://chatgpt.com/backend-api/codex", {},
        "hermes-auth-store"});
    nlohmann::json cfg = nlohmann::json::object();
    auto r = resolve_runtime_provider("gpt-5.3-codex", cfg, &pool);
    EXPECT_EQ(r.provider_name, "openai-codex");
    EXPECT_EQ(r.api_mode, "codex_responses");
}

TEST(ResolveRuntimeProvider, LocalhostBaseUrlAllowsEmptyKey) {
    scrub_api_keys();
    CredentialPool pool;
    nlohmann::json cfg = {{"model",
                           {{"provider", "custom"},
                            {"base_url", "http://localhost:11434/v1"}}}};
    auto r = resolve_runtime_provider("llama3.2", cfg, &pool);
    EXPECT_EQ(r.provider_name, "custom");
    EXPECT_EQ(r.api_key, "no-key-required");
    EXPECT_EQ(r.source, "local");
}

TEST(ResolveRuntimeProvider, NoCredentialsThrows) {
    scrub_api_keys();
    CredentialPool pool;
    nlohmann::json cfg = nlohmann::json::object();
    EXPECT_THROW(resolve_runtime_provider("gpt-4o", cfg, &pool),
                 std::runtime_error);
}

TEST(ResolveRuntimeProvider, NoCredentialsOkWhenAllowed) {
    scrub_api_keys();
    CredentialPool pool;
    nlohmann::json cfg = nlohmann::json::object();
    auto r = resolve_runtime_provider("gpt-4o", cfg, &pool, true);
    EXPECT_EQ(r.provider_name, "openai");
    EXPECT_TRUE(r.api_key.empty());
}
