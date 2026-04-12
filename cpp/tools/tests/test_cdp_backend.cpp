// Phase 9: Tests for the CDP browser backend.
//
// Tests requiring a real Chrome install are gated behind CDP_TEST=1.
// Without that env var, only unit-level tests (config defaults, error
// handling without Chrome) are executed.

#include "hermes/tools/cdp_backend.hpp"
#include "hermes/tools/simple_ws.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdlib>
#include <thread>

namespace hermes::tools {
namespace {

// ---------- Always-run tests (no Chrome required) ----------------------------

TEST(CdpConfig, DefaultValues) {
    CdpConfig cfg;
    EXPECT_EQ(cfg.chrome_path, "google-chrome");
    EXPECT_EQ(cfg.debug_port, 9222);
    EXPECT_TRUE(cfg.headless);
    EXPECT_TRUE(cfg.user_data_dir.empty());
    EXPECT_TRUE(cfg.extra_args.empty());
}

TEST(CdpBackend, ConstructDoesNotLaunch) {
    CdpConfig cfg;
    cfg.chrome_path = "/nonexistent/chrome";
    CdpBackend backend(cfg);
    // Construction alone should not start a process.
    // Destruction is safe even without launch().
}

TEST(CdpBackend, LaunchFailsWithBadPath) {
    CdpConfig cfg;
    cfg.chrome_path = "/nonexistent/chrome-XXXXX";
    CdpBackend backend(cfg);
    EXPECT_FALSE(backend.launch());
}

TEST(CdpBackend, MakeFactoryReturnsNullWithBadPath) {
    CdpConfig cfg;
    cfg.chrome_path = "/nonexistent/chrome-XXXXX";
    auto ptr = make_cdp_backend(cfg);
    EXPECT_EQ(ptr, nullptr);
}

TEST(CdpBackend, CloseWithoutLaunchIsSafe) {
    CdpConfig cfg;
    cfg.chrome_path = "/nonexistent/chrome-XXXXX";
    CdpBackend backend(cfg);
    backend.close();  // should not crash
}

TEST(SimpleWs, InvalidUrlReturnsError) {
    auto resp = ws_send_recv("http://invalid", "hello",
                             std::chrono::seconds(1));
    EXPECT_FALSE(resp.success);
    EXPECT_FALSE(resp.error.empty());
}

TEST(SimpleWs, ConnectionRefusedReturnsError) {
    // Port 19999 is very unlikely to have a listener.
    auto resp = ws_send_recv("ws://127.0.0.1:19999/test", "hello",
                             std::chrono::seconds(2));
    EXPECT_FALSE(resp.success);
    EXPECT_FALSE(resp.error.empty());
}

// ---------- Integration tests (require Chrome + CDP_TEST=1) ------------------

class CdpIntegrationTest : public ::testing::Test {
protected:
    void SetUp() override {
        const char* env = std::getenv("CDP_TEST");
        if (!env || std::string(env) != "1") {
            GTEST_SKIP() << "CDP_TEST=1 not set; skipping Chrome tests";
        }
        CdpConfig cfg;
        cfg.headless = true;
        // Use a non-default port to avoid collisions.
        cfg.debug_port = 9333;
        backend_ = std::make_unique<CdpBackend>(cfg);
        ASSERT_TRUE(backend_->launch())
            << "Chrome failed to launch — is it installed?";
    }

    void TearDown() override {
        if (backend_) backend_->close();
    }

    std::unique_ptr<CdpBackend> backend_;
};

TEST_F(CdpIntegrationTest, NavigateToDataUrl) {
    EXPECT_TRUE(
        backend_->navigate("data:text/html,<h1>Hello</h1>"));
}

TEST_F(CdpIntegrationTest, SnapshotContainsHello) {
    backend_->navigate("data:text/html,<h1>Hello</h1>");
    auto snap = backend_->snapshot();
    bool found = false;
    for (const auto& el : snap.elements) {
        if (el.text.find("Hello") != std::string::npos) {
            found = true;
            break;
        }
    }
    EXPECT_TRUE(found) << "snapshot should contain element with 'Hello'";
}

TEST_F(CdpIntegrationTest, EvaluateJsOnePlusOne) {
    backend_->navigate("about:blank");
    auto result = backend_->evaluate_js("1+1");
    EXPECT_FALSE(result.is_error);
    EXPECT_EQ(result.value, "2");
}

TEST_F(CdpIntegrationTest, ScreenshotReturnsBase64) {
    backend_->navigate("data:text/html,<h1>Screenshot</h1>");
    auto b64 = backend_->screenshot_base64();
    EXPECT_FALSE(b64.empty());
    // Base64-encoded PNG starts with "iVBOR"
    EXPECT_TRUE(b64.substr(0, 5) == "iVBOR")
        << "Expected PNG base64, got: " << b64.substr(0, 20);
}

TEST_F(CdpIntegrationTest, ClickElement) {
    backend_->navigate(
        "data:text/html,"
        "<button id='btn' onclick='document.title=\"clicked\"'>Click me</button>");
    auto snap = backend_->snapshot();
    // Find the button ref
    std::string btn_ref;
    for (const auto& el : snap.elements) {
        if (el.text.find("Click me") != std::string::npos) {
            btn_ref = el.ref;
            break;
        }
    }
    ASSERT_FALSE(btn_ref.empty()) << "button not found in snapshot";
    EXPECT_TRUE(backend_->click(btn_ref, false));

    // Give the click a moment to process
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    auto result = backend_->evaluate_js("document.title");
    EXPECT_EQ(result.value, "clicked");
}

TEST_F(CdpIntegrationTest, GetImageUrls) {
    backend_->navigate(
        "data:text/html,<img src='https://example.com/img.png'>");
    auto urls = backend_->get_image_urls();
    ASSERT_EQ(urls.size(), 1u);
    EXPECT_EQ(urls[0], "https://example.com/img.png");
}

TEST_F(CdpIntegrationTest, GoBack) {
    backend_->navigate("data:text/html,<h1>Page1</h1>");
    backend_->navigate("data:text/html,<h1>Page2</h1>");
    EXPECT_TRUE(backend_->go_back());
}

TEST_F(CdpIntegrationTest, CloseKillsProcess) {
    backend_->close();
    // After close, operations should fail gracefully.
    auto snap = backend_->snapshot();
    EXPECT_TRUE(snap.elements.empty());
}

}  // namespace
}  // namespace hermes::tools
