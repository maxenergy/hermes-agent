#include "hermes/tools/browser_backend.hpp"

namespace hermes::tools {

namespace {
std::unique_ptr<BrowserBackend> g_browser_backend;
}  // namespace

// -- FakeBrowserBackend implementation ------------------------------------

bool FakeBrowserBackend::navigate(const std::string& url) {
    action_log.push_back("navigate:" + url);
    return true;
}

PageSnapshot FakeBrowserBackend::snapshot() {
    action_log.push_back("snapshot");
    return fake_snapshot;
}

bool FakeBrowserBackend::click(const std::string& ref, bool dbl_click) {
    std::string entry = "click:" + ref;
    if (dbl_click) entry += ":dbl";
    action_log.push_back(entry);
    return true;
}

bool FakeBrowserBackend::type(const std::string& ref, const std::string& text,
                              bool submit) {
    std::string entry = "type:" + ref + ":" + text;
    if (submit) entry += ":submit";
    action_log.push_back(entry);
    return true;
}

bool FakeBrowserBackend::scroll(const std::string& direction) {
    action_log.push_back("scroll:" + direction);
    return true;
}

bool FakeBrowserBackend::go_back() {
    action_log.push_back("go_back");
    return true;
}

bool FakeBrowserBackend::press_key(const std::string& key) {
    action_log.push_back("press_key:" + key);
    return true;
}

std::vector<std::string> FakeBrowserBackend::get_image_urls() {
    action_log.push_back("get_image_urls");
    return fake_image_urls;
}

ConsoleResult FakeBrowserBackend::evaluate_js(const std::string& expression) {
    action_log.push_back("evaluate_js:" + expression);
    return fake_console_result;
}

std::string FakeBrowserBackend::screenshot_base64() {
    action_log.push_back("screenshot_base64");
    return fake_screenshot;
}

// -- Global accessor ------------------------------------------------------

void set_browser_backend(std::unique_ptr<BrowserBackend> backend) {
    g_browser_backend = std::move(backend);
}

BrowserBackend* get_browser_backend() {
    return g_browser_backend.get();
}

}  // namespace hermes::tools
