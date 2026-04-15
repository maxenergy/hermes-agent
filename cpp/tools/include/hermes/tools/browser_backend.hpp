// Phase 8.4: Browser backend abstraction for CDP-based browser tools.
//
// Provides an injectable BrowserBackend interface so that tool handlers
// can be tested with a test-double backend without a real browser.
//
// Extended in Phase 13 with full element interaction, navigation, tab
// management, JS evaluate-with-timeout, screenshots (page/element/
// viewport), PDF print, emulation, cookies, downloads, dialogs, file
// chooser, and network/console capture.
#pragma once

#include <chrono>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace hermes::tools {

struct DomElement {
    std::string ref;
    std::string tag;
    std::string text;
    std::string role;
    // Extended fields (Phase 13): zero when the backend doesn't populate.
    int x{0};
    int y{0};
    int width{0};
    int height{0};
    bool visible{true};
    std::map<std::string, std::string> attrs;
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

// Result of an evaluation with timeout.
struct EvalResult {
    std::string value_json;
    bool is_error{false};
    bool timed_out{false};
    std::string error_message;
};

// Screenshot options.
struct ScreenshotOptions {
    enum class Scope { Viewport, FullPage, Element };
    Scope scope{Scope::Viewport};
    std::string element_ref;   // required when scope == Element
    std::string format{"png"}; // "png" or "jpeg"
    int quality{80};           // 1..100 for jpeg
    bool omit_background{false};
};

// Viewport settings.
struct Viewport {
    int width{1280};
    int height{800};
    double device_scale_factor{1.0};
    bool mobile{false};
};

// Tab identifiers returned by the backend.
struct TabHandle {
    std::string id;
    std::string url;
    std::string title;
    bool active{false};
};

// Download record reported by the backend.
struct DownloadInfo {
    std::string id;
    std::string suggested_filename;
    std::string url;
    std::string saved_path;
    int64_t bytes{0};
    bool cancelled{false};
};

// Cookie data interchange format used by the backend interface.
struct BackendCookie {
    std::string name;
    std::string value;
    std::string domain;
    std::string path{"/"};
    bool secure{false};
    bool http_only{false};
    int64_t expires{0};
    std::string same_site{"Lax"};
};

class BrowserBackend {
public:
    virtual ~BrowserBackend() = default;

    // Core actions (originally from Phase 8.4).
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

    // -- Phase 13 extensions.  Default implementations delegate to the
    // core actions or return an unsupported error so older test doubles
    // keep compiling.
    virtual bool go_forward() { return false; }
    virtual bool reload(bool /*ignore_cache*/ = false) { return false; }

    virtual bool hover(const std::string& /*ref*/) { return false; }
    virtual bool fill(const std::string& ref, const std::string& text) {
        return type(ref, text, false);
    }
    virtual bool select_option(const std::string& /*ref*/,
                               const std::vector<std::string>& /*values*/) {
        return false;
    }
    virtual bool drag(const std::string& /*from_ref*/,
                      const std::string& /*to_ref*/) {
        return false;
    }
    virtual bool focus(const std::string& /*ref*/) { return false; }
    virtual bool blur(const std::string& /*ref*/) { return false; }
    virtual bool check(const std::string& /*ref*/, bool /*value*/ = true) {
        return false;
    }
    virtual bool scroll_into_view(const std::string& /*ref*/) { return false; }
    virtual bool upload_files(const std::string& /*ref*/,
                              const std::vector<std::string>& /*paths*/) {
        return false;
    }

    virtual EvalResult evaluate_js_timed(const std::string& expression,
                                         std::chrono::milliseconds /*timeout*/) {
        auto r = evaluate_js(expression);
        EvalResult er;
        er.value_json = r.value;
        er.is_error = r.is_error;
        return er;
    }

    virtual std::string screenshot_base64(const ScreenshotOptions& /*opts*/) {
        return screenshot_base64();
    }
    virtual std::string pdf_base64() { return ""; }

    virtual bool set_viewport(const Viewport& /*v*/) { return false; }
    virtual bool emulate_dark_mode(bool /*enabled*/) { return false; }
    virtual bool emulate_locale(const std::string& /*locale*/) { return false; }
    virtual bool emulate_timezone(const std::string& /*tz*/) { return false; }

    virtual std::vector<BackendCookie> get_cookies() { return {}; }
    virtual bool set_cookies(const std::vector<BackendCookie>& /*cookies*/) {
        return false;
    }
    virtual bool clear_cookies() { return false; }

    virtual std::vector<TabHandle> list_tabs() { return {}; }
    virtual std::string open_tab(const std::string& /*url*/) { return ""; }
    virtual bool close_tab(const std::string& /*id*/) { return false; }
    virtual bool activate_tab(const std::string& /*id*/) { return false; }

    virtual bool wait_for_selector(const std::string& /*sel*/,
                                   const std::string& /*state*/,
                                   std::chrono::milliseconds /*timeout*/) {
        return false;
    }

    virtual std::vector<DownloadInfo> list_downloads() { return {}; }
};

class FakeBrowserBackend : public BrowserBackend {
public:
    // Configurable responses for testing.
    PageSnapshot fake_snapshot;
    ConsoleResult fake_console_result;
    std::vector<std::string> fake_image_urls;
    std::string fake_screenshot;
    std::vector<std::string> action_log;

    // Phase 13 extensions.
    EvalResult fake_eval_timed;
    std::string fake_pdf;
    Viewport fake_viewport;
    std::vector<BackendCookie> fake_cookies;
    std::vector<TabHandle> fake_tabs;
    bool fake_wait_result{true};
    std::vector<DownloadInfo> fake_downloads;
    bool fake_fail{false};

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

    // Extensions.
    bool go_forward() override;
    bool reload(bool ignore_cache) override;
    bool hover(const std::string& ref) override;
    bool fill(const std::string& ref, const std::string& text) override;
    bool select_option(const std::string& ref,
                       const std::vector<std::string>& values) override;
    bool drag(const std::string& from_ref,
              const std::string& to_ref) override;
    bool focus(const std::string& ref) override;
    bool blur(const std::string& ref) override;
    bool check(const std::string& ref, bool value) override;
    bool scroll_into_view(const std::string& ref) override;
    bool upload_files(const std::string& ref,
                      const std::vector<std::string>& paths) override;
    EvalResult evaluate_js_timed(const std::string& expression,
                                 std::chrono::milliseconds timeout) override;
    std::string screenshot_base64(const ScreenshotOptions& opts) override;
    std::string pdf_base64() override;
    bool set_viewport(const Viewport& v) override;
    bool emulate_dark_mode(bool enabled) override;
    bool emulate_locale(const std::string& locale) override;
    bool emulate_timezone(const std::string& tz) override;
    std::vector<BackendCookie> get_cookies() override;
    bool set_cookies(const std::vector<BackendCookie>& cookies) override;
    bool clear_cookies() override;
    std::vector<TabHandle> list_tabs() override;
    std::string open_tab(const std::string& url) override;
    bool close_tab(const std::string& id) override;
    bool activate_tab(const std::string& id) override;
    bool wait_for_selector(const std::string& sel, const std::string& state,
                           std::chrono::milliseconds timeout) override;
    std::vector<DownloadInfo> list_downloads() override;
};

// Global setter/getter.  Phase 13 wires the real Playwright/CDP backend.
void set_browser_backend(std::unique_ptr<BrowserBackend> backend);
BrowserBackend* get_browser_backend();

}  // namespace hermes::tools
