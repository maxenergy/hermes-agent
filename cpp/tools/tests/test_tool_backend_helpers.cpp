#include "hermes/tools/tool_backend_helpers.hpp"

#include <gtest/gtest.h>

#include <cstdlib>

namespace bh = hermes::tools::backend_helpers;

TEST(BackendHelpers, NormalizeBrowserProviderDefaultsToLocal) {
    EXPECT_EQ(bh::normalize_browser_cloud_provider(""), "local");
    EXPECT_EQ(bh::normalize_browser_cloud_provider("  Daytona "), "daytona");
}

TEST(BackendHelpers, CoerceModalModeClamps) {
    EXPECT_EQ(bh::coerce_modal_mode("managed"), "managed");
    EXPECT_EQ(bh::coerce_modal_mode("DIRECT"), "direct");
    EXPECT_EQ(bh::coerce_modal_mode("auto"), "auto");
    EXPECT_EQ(bh::coerce_modal_mode(""), "auto");
    EXPECT_EQ(bh::coerce_modal_mode("bogus"), "auto");
}

TEST(BackendHelpers, ResolveModalAutoPrefersManagedWhenReady) {
    ::setenv("HERMES_ENABLE_NOUS_MANAGED_TOOLS", "1", 1);
    auto s = bh::resolve_modal_backend_state("auto", /*has_direct=*/true,
                                             /*managed_ready=*/true);
    ASSERT_TRUE(s.selected_backend.has_value());
    EXPECT_EQ(*s.selected_backend, "managed");
    ::unsetenv("HERMES_ENABLE_NOUS_MANAGED_TOOLS");
}

TEST(BackendHelpers, ResolveModalDirectModeBlocksManaged) {
    ::unsetenv("HERMES_ENABLE_NOUS_MANAGED_TOOLS");
    auto s = bh::resolve_modal_backend_state("direct", /*has_direct=*/true,
                                             /*managed_ready=*/true);
    ASSERT_TRUE(s.selected_backend.has_value());
    EXPECT_EQ(*s.selected_backend, "direct");
}

TEST(BackendHelpers, ResolveModalManagedWithoutFlagIsBlocked) {
    ::unsetenv("HERMES_ENABLE_NOUS_MANAGED_TOOLS");
    auto s = bh::resolve_modal_backend_state("managed", /*has_direct=*/true,
                                             /*managed_ready=*/true);
    EXPECT_TRUE(s.managed_mode_blocked);
    EXPECT_FALSE(s.selected_backend.has_value());
}

TEST(BackendHelpers, SelectBackendHonoursMockList) {
    nlohmann::json cfg;
    cfg["mock_tools"] = {"web_search", "firecrawl"};
    EXPECT_EQ(bh::select_backend("web_search", cfg), "mock");
    EXPECT_EQ(bh::select_backend("other", cfg), "default");
}

TEST(BackendHelpers, ApplyManagedGatewayHeadersAddsNousClient) {
    std::unordered_map<std::string, std::string> headers;
    nlohmann::json cfg;
    cfg["nous_api_key"] = "secret";
    bh::apply_backend_headers(headers, "managed_gateway", cfg);
    EXPECT_EQ(headers["X-Nous-Client"], "hermes-cpp");
    EXPECT_EQ(headers["X-Nous-API-Key"], "secret");
}

TEST(BackendHelpers, ResolveOpenAIAudioKeyPrefersVoiceKey) {
    ::setenv("VOICE_TOOLS_OPENAI_KEY", "voice-key", 1);
    ::setenv("OPENAI_API_KEY", "main-key", 1);
    EXPECT_EQ(bh::resolve_openai_audio_api_key(), "voice-key");
    ::unsetenv("VOICE_TOOLS_OPENAI_KEY");
    EXPECT_EQ(bh::resolve_openai_audio_api_key(), "main-key");
    ::unsetenv("OPENAI_API_KEY");
}

TEST(BackendHelpers, ResolveVendorEndpointUsesGatewayBase) {
    nlohmann::json cfg;
    cfg["managed_gateway_base"] = "https://gw.example.com/";
    auto url = bh::resolve_vendor_endpoint("firecrawl", cfg);
    ASSERT_TRUE(url.has_value());
    EXPECT_EQ(*url, "https://gw.example.com/firecrawl");
}
