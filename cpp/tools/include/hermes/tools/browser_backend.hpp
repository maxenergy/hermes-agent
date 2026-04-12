// Phase 8.4: Browser backend abstraction for CDP-based browser tools.
//
// Provides an injectable BrowserBackend interface so that tool handlers
// can be tested with a test-double backend without a real browser.
#pragma once

#include <memory>
#include <string>
#include <vector>

namespace hermes::tools {

struct DomElement {
    std::string ref;
    std::string tag;
    std::string text;
    std::string role;
};

struct PageSnapshot {
    std::string url;
    std::string title;
    std::vector<DomElement> elements;
};

struct ConsoleResult {
    std::string value;
    bool is_error;
};

class BrowserBackend {
public:
    virtual ~BrowserBackend() = default;
    virtual bool navigate(const std::string& url) = 0;
    virtual PageSnapshot snapshot() = 0;
    virtual bool click(const std::string& ref, bool dbl_click = false) = 0;
    virtual bool type(const std::string& ref, const std::string& text,
                      bool submit = false) = 0;
    virtual bool scroll(const std::string& direction) = 0;
    virtual bool go_back() = 0;
    virtual bool press_key(const std::string& key) = 0;
    virtual std::vector<std::string> get_image_urls() = 0;
    virtual ConsoleResult evaluate_js(const std::string& expression) = 0;
    virtual std::string screenshot_base64() = 0;
};

class FakeBrowserBackend : public BrowserBackend {
public:
    // Configurable responses for testing.
    PageSnapshot fake_snapshot;
    ConsoleResult fake_console_result;
    std::vector<std::string> fake_image_urls;
    std::string fake_screenshot;
    std::vector<std::string> action_log;

    bool navigate(const std::string& url) override;
    PageSnapshot snapshot() override;
    bool click(const std::string& ref, bool dbl_click) override;
    bool type(const std::string& ref, const std::string& text,
              bool submit) override;
    bool scroll(const std::string& direction) override;
    bool go_back() override;
    bool press_key(const std::string& key) override;
    std::vector<std::string> get_image_urls() override;
    ConsoleResult evaluate_js(const std::string& expression) override;
    std::string screenshot_base64() override;
};

// Global setter/getter.  Phase 13 wires the real Playwright/CDP backend.
void set_browser_backend(std::unique_ptr<BrowserBackend> backend);
BrowserBackend* get_browser_backend();

}  // namespace hermes::tools
