#include "hermes/tools/browser_backend.hpp"

#include <sstream>

namespace hermes::tools {

namespace {
std::unique_ptr<BrowserBackend> g_browser_backend;
}  // namespace

// -- FakeBrowserBackend implementation ------------------------------------

bool FakeBrowserBackend::navigate(const std::string& url) {
    action_log.push_back("navigate:" + url);
    return !fake_fail;
}

PageSnapshot FakeBrowserBackend::snapshot() {
    action_log.push_back("snapshot");
    return fake_snapshot;
}

bool FakeBrowserBackend::click(const std::string& ref, bool dbl_click) {
    std::string entry = "click:" + ref;
    if (dbl_click) entry += ":dbl";
    action_log.push_back(entry);
    return !fake_fail;
}

bool FakeBrowserBackend::type(const std::string& ref, const std::string& text,
                              bool submit) {
    std::string entry = "type:" + ref + ":" + text;
    if (submit) entry += ":submit";
    action_log.push_back(entry);
    return !fake_fail;
}

bool FakeBrowserBackend::scroll(const std::string& direction) {
    action_log.push_back("scroll:" + direction);
    return !fake_fail;
}

bool FakeBrowserBackend::go_back() {
    action_log.push_back("go_back");
    return !fake_fail;
}

bool FakeBrowserBackend::press_key(const std::string& key) {
    action_log.push_back("press_key:" + key);
    return !fake_fail;
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

// -- Extensions -----------------------------------------------------------

bool FakeBrowserBackend::go_forward() {
    action_log.push_back("go_forward");
    return !fake_fail;
}

bool FakeBrowserBackend::reload(bool ignore_cache) {
    action_log.push_back(ignore_cache ? "reload:ignore_cache" : "reload");
    return !fake_fail;
}

bool FakeBrowserBackend::hover(const std::string& ref) {
    action_log.push_back("hover:" + ref);
    return !fake_fail;
}

bool FakeBrowserBackend::fill(const std::string& ref, const std::string& text) {
    action_log.push_back("fill:" + ref + ":" + text);
    return !fake_fail;
}

bool FakeBrowserBackend::select_option(const std::string& ref,
                                       const std::vector<std::string>& values) {
    std::ostringstream os;
    os << "select:" << ref;
    for (const auto& v : values) os << ":" << v;
    action_log.push_back(os.str());
    return !fake_fail;
}

bool FakeBrowserBackend::drag(const std::string& from_ref,
                              const std::string& to_ref) {
    action_log.push_back("drag:" + from_ref + "->" + to_ref);
    return !fake_fail;
}

bool FakeBrowserBackend::focus(const std::string& ref) {
    action_log.push_back("focus:" + ref);
    return !fake_fail;
}

bool FakeBrowserBackend::blur(const std::string& ref) {
    action_log.push_back("blur:" + ref);
    return !fake_fail;
}

bool FakeBrowserBackend::check(const std::string& ref, bool value) {
    action_log.push_back(std::string("check:") + ref + ":" +
                         (value ? "true" : "false"));
    return !fake_fail;
}

bool FakeBrowserBackend::scroll_into_view(const std::string& ref) {
    action_log.push_back("scroll_into_view:" + ref);
    return !fake_fail;
}

bool FakeBrowserBackend::upload_files(const std::string& ref,
                                      const std::vector<std::string>& paths) {
    std::ostringstream os;
    os << "upload:" << ref;
    for (const auto& p : paths) os << ":" << p;
    action_log.push_back(os.str());
    return !fake_fail;
}

EvalResult FakeBrowserBackend::evaluate_js_timed(const std::string& expression,
                                                 std::chrono::milliseconds) {
    action_log.push_back("evaluate_js_timed:" + expression);
    return fake_eval_timed;
}

std::string FakeBrowserBackend::screenshot_base64(const ScreenshotOptions& opts) {
    std::ostringstream os;
    os << "screenshot_opts:";
    switch (opts.scope) {
        case ScreenshotOptions::Scope::Viewport: os << "viewport"; break;
        case ScreenshotOptions::Scope::FullPage: os << "fullpage"; break;
        case ScreenshotOptions::Scope::Element:
            os << "element:" << opts.element_ref;
            break;
    }
    os << ":" << opts.format;
    action_log.push_back(os.str());
    return fake_screenshot;
}

std::string FakeBrowserBackend::pdf_base64() {
    action_log.push_back("pdf_base64");
    return fake_pdf;
}

bool FakeBrowserBackend::set_viewport(const Viewport& v) {
    fake_viewport = v;
    std::ostringstream os;
    os << "set_viewport:" << v.width << "x" << v.height;
    action_log.push_back(os.str());
    return !fake_fail;
}

bool FakeBrowserBackend::emulate_dark_mode(bool e) {
    action_log.push_back(std::string("dark_mode:") + (e ? "on" : "off"));
    return !fake_fail;
}

bool FakeBrowserBackend::emulate_locale(const std::string& l) {
    action_log.push_back("locale:" + l);
    return !fake_fail;
}

bool FakeBrowserBackend::emulate_timezone(const std::string& tz) {
    action_log.push_back("timezone:" + tz);
    return !fake_fail;
}

std::vector<BackendCookie> FakeBrowserBackend::get_cookies() {
    action_log.push_back("get_cookies");
    return fake_cookies;
}

bool FakeBrowserBackend::set_cookies(const std::vector<BackendCookie>& c) {
    fake_cookies = c;
    action_log.push_back("set_cookies:" + std::to_string(c.size()));
    return !fake_fail;
}

bool FakeBrowserBackend::clear_cookies() {
    fake_cookies.clear();
    action_log.push_back("clear_cookies");
    return !fake_fail;
}

std::vector<TabHandle> FakeBrowserBackend::list_tabs() {
    action_log.push_back("list_tabs");
    return fake_tabs;
}

std::string FakeBrowserBackend::open_tab(const std::string& url) {
    TabHandle t;
    t.id = "tab-" + std::to_string(fake_tabs.size() + 1);
    t.url = url;
    for (auto& tab : fake_tabs) tab.active = false;
    t.active = true;
    fake_tabs.push_back(t);
    action_log.push_back("open_tab:" + url);
    return t.id;
}

bool FakeBrowserBackend::close_tab(const std::string& id) {
    action_log.push_back("close_tab:" + id);
    for (auto it = fake_tabs.begin(); it != fake_tabs.end(); ++it) {
        if (it->id == id) {
            fake_tabs.erase(it);
            return true;
        }
    }
    return false;
}

bool FakeBrowserBackend::activate_tab(const std::string& id) {
    action_log.push_back("activate_tab:" + id);
    bool found = false;
    for (auto& t : fake_tabs) {
        t.active = (t.id == id);
        if (t.active) found = true;
    }
    return found;
}

bool FakeBrowserBackend::wait_for_selector(const std::string& sel,
                                           const std::string& state,
                                           std::chrono::milliseconds) {
    action_log.push_back("wait:" + sel + ":" + state);
    return fake_wait_result;
}

std::vector<DownloadInfo> FakeBrowserBackend::list_downloads() {
    action_log.push_back("list_downloads");
    return fake_downloads;
}

// -- Global accessor ------------------------------------------------------

void set_browser_backend(std::unique_ptr<BrowserBackend> backend) {
    g_browser_backend = std::move(backend);
}

BrowserBackend* get_browser_backend() {
    return g_browser_backend.get();
}

}  // namespace hermes::tools
