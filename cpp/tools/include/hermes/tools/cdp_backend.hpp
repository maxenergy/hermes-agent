// Phase 9: Real Chrome DevTools Protocol browser backend.
//
// Launches a Chromium process with --remote-debugging-port, communicates
// over CDP via a minimal WebSocket client (simple_ws.hpp), and implements
// the BrowserBackend interface so the 10 browser tools work against a
// real browser.
#pragma once

#include "hermes/tools/browser_backend.hpp"

#include <memory>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace hermes::tools {

struct CdpConfig {
    std::string chrome_path = "google-chrome";
    int debug_port = 9222;
    bool headless = true;
    std::string user_data_dir;  // empty = auto temp dir
    std::vector<std::string> extra_args;
};

class CdpBackend : public BrowserBackend {
public:
    explicit CdpBackend(CdpConfig config);
    ~CdpBackend() override;

    /// Launch Chrome with remote debugging.  Returns false on failure.
    bool launch();

    /// Kill the Chrome process.
    void close();

    // -- BrowserBackend interface --
    bool navigate(const std::string& url) override;
    PageSnapshot snapshot() override;
    bool click(const std::string& ref, bool dbl_click = false) override;
    bool type(const std::string& ref, const std::string& text,
              bool submit = false) override;
    bool scroll(const std::string& direction) override;
    bool go_back() override;
    bool press_key(const std::string& key) override;
    std::vector<std::string> get_image_urls() override;
    ConsoleResult evaluate_js(const std::string& expression) override;
    std::string screenshot_base64() override;

private:
    CdpConfig config_;
    pid_t chrome_pid_ = -1;
    std::string temp_dir_;   // auto-created user data dir (if config_.user_data_dir empty)
    std::string tab_ws_url_; // WebSocket debugger URL for the active tab

    /// Discover tabs via HTTP GET localhost:debug_port/json and cache ws URL.
    bool discover_tab();

    /// Send a CDP command to the active tab and return the response.
    nlohmann::json send_command(const std::string& method,
                                const nlohmann::json& params = {});

    /// Resolve a ref string (e.g. "e5") to a DOM nodeId.
    int resolve_ref(const std::string& ref);
};

/// Factory: create and launch a CDP backend.  Returns nullptr on launch failure.
std::unique_ptr<BrowserBackend> make_cdp_backend(CdpConfig config = {});

}  // namespace hermes::tools
