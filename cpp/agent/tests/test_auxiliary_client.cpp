#include "hermes/agent/auxiliary_client.hpp"

#include "hermes/llm/llm_client.hpp"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

using hermes::agent::get_auxiliary_model;
using hermes::agent::make_auxiliary_client;
using hermes::llm::CompletionRequest;
using hermes::llm::FakeHttpTransport;
using json = nlohmann::json;

TEST(AuxiliaryClient, MissingConfigReturnsThrowingStub) {
    FakeHttpTransport tr;
    auto cli = make_auxiliary_client(json::object(), &tr);
    ASSERT_TRUE(cli);
    CompletionRequest req;
    req.model = "x";
    EXPECT_THROW(cli->complete(req), std::runtime_error);
}

TEST(AuxiliaryClient, OpenAIProviderConstructsClient) {
    FakeHttpTransport tr;
    json cfg;
    cfg["auxiliary"]["provider"] = "openai";
    cfg["auxiliary"]["model"] = "gpt-4o-mini";
    cfg["auxiliary"]["api_key"] = "sk-test";
    auto cli = make_auxiliary_client(cfg, &tr);
    ASSERT_TRUE(cli);
    EXPECT_EQ(cli->provider_name(), "openai");
}

TEST(AuxiliaryClient, AnthropicProviderConstructsClient) {
    FakeHttpTransport tr;
    json cfg;
    cfg["auxiliary"]["provider"] = "anthropic";
    cfg["auxiliary"]["api_key"] = "sk-ant";
    auto cli = make_auxiliary_client(cfg, &tr);
    ASSERT_TRUE(cli);
    EXPECT_EQ(cli->provider_name(), "anthropic");
}

TEST(AuxiliaryClient, UnknownProviderReturnsStub) {
    FakeHttpTransport tr;
    json cfg;
    cfg["auxiliary"]["provider"] = "deepseek-but-not-yet";
    auto cli = make_auxiliary_client(cfg, &tr);
    ASSERT_TRUE(cli);
    CompletionRequest req;
    req.model = "x";
    EXPECT_THROW(cli->complete(req), std::runtime_error);
}

TEST(AuxiliaryClient, GetAuxiliaryModelReadsField) {
    json cfg;
    cfg["auxiliary"]["model"] = "gpt-4o-mini";
    EXPECT_EQ(get_auxiliary_model(cfg), "gpt-4o-mini");
    EXPECT_EQ(get_auxiliary_model(json::object()), "");
}
