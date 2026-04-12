// Phase 9: Real Chrome DevTools Protocol browser backend.
//
// Launches Chrome with --remote-debugging-port, discovers tabs via HTTP,
// and sends CDP commands over WebSocket (simple_ws.hpp).

#include "hermes/tools/cdp_backend.hpp"
#include "hermes/tools/simple_ws.hpp"

#include <nlohmann/json.hpp>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace hermes::tools {

namespace {

/// Monotonically increasing CDP command id.
std::atomic<int> g_cdp_id{1};

/// Simple HTTP GET to localhost:port/path.  Returns response body or "".
std::string http_get(const std::string& host, int port,
                     const std::string& path, int timeout_sec = 5) {
    struct addrinfo hints {};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    struct addrinfo* res = nullptr;
    if (::getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints,
                      &res) != 0 ||
        !res) {
        return {};
    }

    int fd = ::socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (fd < 0) {
        ::freeaddrinfo(res);
        return {};
    }

    struct timeval tv {};
    tv.tv_sec = timeout_sec;
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    if (::connect(fd, res->ai_addr, res->ai_addrlen) < 0) {
        ::freeaddrinfo(res);
        ::close(fd);
        return {};
    }
    ::freeaddrinfo(res);

    std::string req = "GET " + path +
                      " HTTP/1.1\r\n"
                      "Host: " +
                      host + ":" + std::to_string(port) +
                      "\r\n"
                      "Connection: close\r\n"
                      "\r\n";
    if (::write(fd, req.data(), req.size()) !=
        static_cast<ssize_t>(req.size())) {
        ::close(fd);
        return {};
    }

    std::string response;
    char buf[4096];
    while (true) {
        auto n = ::read(fd, buf, sizeof(buf));
        if (n <= 0) break;
        response.append(buf, static_cast<size_t>(n));
    }
    ::close(fd);

    // Strip HTTP headers
    auto hdr_end = response.find("\r\n\r\n");
    if (hdr_end == std::string::npos) return {};
    return response.substr(hdr_end + 4);
}

/// Check if a string looks like it could be an executable on PATH.
bool executable_exists(const std::string& name) {
    // If absolute path, check directly.
    if (!name.empty() && name[0] == '/') {
        return ::access(name.c_str(), X_OK) == 0;
    }
    // Search PATH
    const char* path_env = ::getenv("PATH");
    if (!path_env) return false;
    std::istringstream ss(path_env);
    std::string dir;
    while (std::getline(ss, dir, ':')) {
        std::string full = dir + "/" + name;
        if (::access(full.c_str(), X_OK) == 0) return true;
    }
    return false;
}

/// Create a temporary directory using mkdtemp.
std::string make_temp_dir() {
    std::string tmpl = "/tmp/hermes-chrome-XXXXXX";
    std::vector<char> buf(tmpl.begin(), tmpl.end());
    buf.push_back('\0');
    char* result = ::mkdtemp(buf.data());
    if (!result) return {};
    return std::string(result);
}

/// Recursively remove a directory (best-effort, small dirs only).
void remove_dir(const std::string& path) {
    if (path.empty() || path == "/") return;
    // Use system rm for simplicity (this is a temp dir we created).
    std::string cmd = "rm -rf '" + path + "' 2>/dev/null";
    int _rc = ::system(cmd.c_str());
    (void)_rc;
}

}  // namespace

// -- CdpBackend implementation ------------------------------------------------

CdpBackend::CdpBackend(CdpConfig config) : config_(std::move(config)) {}

CdpBackend::~CdpBackend() { close(); }

bool CdpBackend::launch() {
    // Already running?
    if (chrome_pid_ > 0) return true;

    // Check Chrome executable exists
    if (!executable_exists(config_.chrome_path)) {
        // Try common alternatives
        for (const auto& alt :
             {"chromium", "chromium-browser", "google-chrome-stable"}) {
            if (executable_exists(alt)) {
                config_.chrome_path = alt;
                break;
            }
        }
        if (!executable_exists(config_.chrome_path)) return false;
    }

    // Create temp user data dir if needed
    if (config_.user_data_dir.empty()) {
        temp_dir_ = make_temp_dir();
        if (temp_dir_.empty()) return false;
    }
    const std::string& data_dir =
        config_.user_data_dir.empty() ? temp_dir_ : config_.user_data_dir;

    // Build args
    std::vector<std::string> args;
    args.push_back(config_.chrome_path);
    if (config_.headless) {
        args.push_back("--headless");
    }
    args.push_back("--disable-gpu");
    args.push_back("--remote-debugging-port=" +
                   std::to_string(config_.debug_port));
    args.push_back("--no-first-run");
    args.push_back("--no-default-browser-check");
    args.push_back("--disable-extensions");
    args.push_back("--disable-background-networking");
    args.push_back("--user-data-dir=" + data_dir);
    for (const auto& extra : config_.extra_args) {
        args.push_back(extra);
    }
    args.push_back("about:blank");

    // Fork + exec
    pid_t pid = ::fork();
    if (pid < 0) return false;

    if (pid == 0) {
        // Child — redirect stdout/stderr to /dev/null
        int devnull = ::open("/dev/null", O_WRONLY);
        if (devnull >= 0) {
            ::dup2(devnull, STDOUT_FILENO);
            ::dup2(devnull, STDERR_FILENO);
            ::close(devnull);
        }
        // Build argv
        std::vector<const char*> argv;
        for (const auto& a : args) argv.push_back(a.c_str());
        argv.push_back(nullptr);
        ::execvp(argv[0], const_cast<char* const*>(argv.data()));
        ::_exit(127);
    }

    chrome_pid_ = pid;

    // Wait for Chrome to be ready (poll /json endpoint)
    for (int attempt = 0; attempt < 50; ++attempt) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        auto body =
            http_get("127.0.0.1", config_.debug_port, "/json", 2);
        if (!body.empty()) {
            // Try to parse and discover tab
            if (discover_tab()) return true;
        }
        // Check if child died
        int status = 0;
        if (::waitpid(chrome_pid_, &status, WNOHANG) > 0) {
            chrome_pid_ = -1;
            return false;
        }
    }

    // Timeout — kill Chrome
    close();
    return false;
}

void CdpBackend::close() {
    if (chrome_pid_ > 0) {
        ::kill(chrome_pid_, SIGTERM);
        // Give it a moment, then SIGKILL
        for (int i = 0; i < 10; ++i) {
            int status = 0;
            if (::waitpid(chrome_pid_, &status, WNOHANG) > 0) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        ::kill(chrome_pid_, SIGKILL);
        int status = 0;
        ::waitpid(chrome_pid_, &status, 0);
        chrome_pid_ = -1;
    }
    tab_ws_url_.clear();

    // Clean up temp dir
    if (!temp_dir_.empty()) {
        remove_dir(temp_dir_);
        temp_dir_.clear();
    }
}

bool CdpBackend::discover_tab() {
    auto body = http_get("127.0.0.1", config_.debug_port, "/json", 2);
    if (body.empty()) return false;

    try {
        auto tabs = nlohmann::json::parse(body);
        if (!tabs.is_array() || tabs.empty()) return false;
        for (const auto& tab : tabs) {
            if (tab.value("type", "") == "page" &&
                tab.contains("webSocketDebuggerUrl")) {
                tab_ws_url_ = tab["webSocketDebuggerUrl"].get<std::string>();
                return true;
            }
        }
        // Fallback: use first entry with a ws URL
        for (const auto& tab : tabs) {
            if (tab.contains("webSocketDebuggerUrl")) {
                tab_ws_url_ = tab["webSocketDebuggerUrl"].get<std::string>();
                return true;
            }
        }
    } catch (...) {
    }
    return false;
}

nlohmann::json CdpBackend::send_command(const std::string& method,
                                        const nlohmann::json& params) {
    if (tab_ws_url_.empty()) {
        if (!discover_tab()) {
            return {{"error", {{"message", "no tab available"}}}};
        }
    }

    nlohmann::json cmd;
    cmd["id"] = g_cdp_id.fetch_add(1);
    cmd["method"] = method;
    if (!params.is_null() && !params.empty()) {
        cmd["params"] = params;
    }

    auto ws_resp = ws_send_recv(tab_ws_url_, cmd.dump(),
                                std::chrono::seconds(30));
    if (!ws_resp.success) {
        return {{"error", {{"message", ws_resp.error}}}};
    }

    try {
        return nlohmann::json::parse(ws_resp.data);
    } catch (...) {
        return {{"error", {{"message", "failed to parse CDP response"}}}};
    }
}

int CdpBackend::resolve_ref(const std::string& ref) {
    // Refs are like "e5" — extract the numeric part as nodeId.
    std::string num;
    for (char c : ref) {
        if (c >= '0' && c <= '9') num.push_back(c);
    }
    if (num.empty()) return 0;
    try {
        return std::stoi(num);
    } catch (...) {
        return 0;
    }
}

// -- BrowserBackend interface -------------------------------------------------

bool CdpBackend::navigate(const std::string& url) {
    auto resp = send_command("Page.navigate", {{"url", url}});
    if (resp.contains("error")) return false;

    // Wait a bit for page load (simple approach — a real impl would
    // wait for Page.loadEventFired via persistent WS connection).
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    // Try to wait for load by polling document.readyState
    for (int i = 0; i < 20; ++i) {
        auto state_resp = send_command(
            "Runtime.evaluate",
            {{"expression", "document.readyState"},
             {"returnByValue", true}});
        try {
            auto val = state_resp.at("result")
                           .at("result")
                           .at("value")
                           .get<std::string>();
            if (val == "complete" || val == "interactive") return true;
        } catch (...) {
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
    return true;  // navigated even if load didn't fully complete
}

PageSnapshot CdpBackend::snapshot() {
    PageSnapshot snap;

    // Get URL and title
    auto url_resp = send_command(
        "Runtime.evaluate",
        {{"expression", "document.URL"}, {"returnByValue", true}});
    try {
        snap.url = url_resp.at("result")
                       .at("result")
                       .at("value")
                       .get<std::string>();
    } catch (...) {
    }

    auto title_resp = send_command(
        "Runtime.evaluate",
        {{"expression", "document.title"}, {"returnByValue", true}});
    try {
        snap.title = title_resp.at("result")
                         .at("result")
                         .at("value")
                         .get<std::string>();
    } catch (...) {
    }

    // Get visible text elements using DOM + Runtime
    auto js = R"JS(
(function() {
    var results = [];
    var all = document.querySelectorAll('*');
    var id = 1;
    for (var i = 0; i < all.length && id <= 200; i++) {
        var el = all[i];
        var tag = el.tagName.toLowerCase();
        var text = '';
        if (el.childNodes.length > 0) {
            for (var j = 0; j < el.childNodes.length; j++) {
                if (el.childNodes[j].nodeType === 3) {
                    text += el.childNodes[j].textContent;
                }
            }
        }
        text = text.trim().substring(0, 200);
        if (text || tag === 'input' || tag === 'button' || tag === 'a' ||
            tag === 'select' || tag === 'textarea') {
            var role = el.getAttribute('role') || '';
            if (!role) {
                if (tag === 'a') role = 'link';
                else if (tag === 'button' || tag === 'input' && el.type === 'submit') role = 'button';
                else if (tag === 'input') role = 'textbox';
                else if (tag === 'h1' || tag === 'h2' || tag === 'h3') role = 'heading';
            }
            results.push({ref: 'e' + id, tag: tag, text: text, role: role});
            el.setAttribute('data-hermes-ref', 'e' + id);
            id++;
        }
    }
    return JSON.stringify(results);
})()
)JS";

    auto dom_resp = send_command(
        "Runtime.evaluate",
        {{"expression", js}, {"returnByValue", true}});
    try {
        auto val = dom_resp.at("result")
                       .at("result")
                       .at("value")
                       .get<std::string>();
        auto elements = nlohmann::json::parse(val);
        for (const auto& el : elements) {
            DomElement de;
            de.ref = el.value("ref", "");
            de.tag = el.value("tag", "");
            de.text = el.value("text", "");
            de.role = el.value("role", "");
            snap.elements.push_back(std::move(de));
        }
    } catch (...) {
    }

    return snap;
}

bool CdpBackend::click(const std::string& ref, bool dbl_click) {
    // Find element position using data-hermes-ref attribute
    std::string js =
        "(function() {"
        "  var el = document.querySelector('[data-hermes-ref=\"" +
        ref +
        "\"]');"
        "  if (!el) return null;"
        "  var r = el.getBoundingClientRect();"
        "  return JSON.stringify({x: r.x + r.width/2, y: r.y + r.height/2});"
        "})()";

    auto resp = send_command("Runtime.evaluate",
                             {{"expression", js}, {"returnByValue", true}});
    double x = 0, y = 0;
    try {
        auto val = resp.at("result")
                       .at("result")
                       .at("value")
                       .get<std::string>();
        auto pos = nlohmann::json::parse(val);
        x = pos["x"].get<double>();
        y = pos["y"].get<double>();
    } catch (...) {
        return false;
    }

    // Dispatch mouse events
    send_command("Input.dispatchMouseEvent",
                 {{"type", "mousePressed"},
                  {"x", x},
                  {"y", y},
                  {"button", "left"},
                  {"clickCount", dbl_click ? 2 : 1}});
    send_command("Input.dispatchMouseEvent",
                 {{"type", "mouseReleased"},
                  {"x", x},
                  {"y", y},
                  {"button", "left"},
                  {"clickCount", dbl_click ? 2 : 1}});

    return true;
}

bool CdpBackend::type(const std::string& ref, const std::string& text,
                      bool submit) {
    // Focus the element
    std::string js =
        "(function() {"
        "  var el = document.querySelector('[data-hermes-ref=\"" +
        ref +
        "\"]');"
        "  if (!el) return false;"
        "  el.focus();"
        "  return true;"
        "})()";

    auto resp = send_command("Runtime.evaluate",
                             {{"expression", js}, {"returnByValue", true}});
    try {
        if (!resp.at("result").at("result").at("value").get<bool>())
            return false;
    } catch (...) {
        return false;
    }

    // Type each character
    for (char c : text) {
        send_command("Input.dispatchKeyEvent",
                     {{"type", "keyDown"},
                      {"text", std::string(1, c)}});
        send_command("Input.dispatchKeyEvent",
                     {{"type", "keyUp"},
                      {"text", std::string(1, c)}});
    }

    if (submit) {
        send_command("Input.dispatchKeyEvent",
                     {{"type", "keyDown"},
                      {"key", "Enter"},
                      {"code", "Enter"},
                      {"windowsVirtualKeyCode", 13}});
        send_command("Input.dispatchKeyEvent",
                     {{"type", "keyUp"},
                      {"key", "Enter"},
                      {"code", "Enter"},
                      {"windowsVirtualKeyCode", 13}});
    }

    return true;
}

bool CdpBackend::scroll(const std::string& direction) {
    int delta_x = 0;
    int delta_y = 0;
    if (direction == "down")
        delta_y = 300;
    else if (direction == "up")
        delta_y = -300;
    else if (direction == "left")
        delta_x = -300;
    else if (direction == "right")
        delta_x = 300;

    send_command("Input.dispatchMouseEvent",
                 {{"type", "mouseWheel"},
                  {"x", 200},
                  {"y", 200},
                  {"deltaX", delta_x},
                  {"deltaY", delta_y}});
    return true;
}

bool CdpBackend::go_back() {
    auto resp = send_command(
        "Runtime.evaluate",
        {{"expression", "history.back()"}, {"returnByValue", true}});
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    return !resp.contains("error");
}

bool CdpBackend::press_key(const std::string& key) {
    // Map common key names to CDP key events
    nlohmann::json params;
    params["key"] = key;

    if (key == "Enter") {
        params["code"] = "Enter";
        params["windowsVirtualKeyCode"] = 13;
    } else if (key == "Tab") {
        params["code"] = "Tab";
        params["windowsVirtualKeyCode"] = 9;
    } else if (key == "Escape") {
        params["code"] = "Escape";
        params["windowsVirtualKeyCode"] = 27;
    } else if (key == "Backspace") {
        params["code"] = "Backspace";
        params["windowsVirtualKeyCode"] = 8;
    } else if (key == "ArrowDown") {
        params["code"] = "ArrowDown";
        params["windowsVirtualKeyCode"] = 40;
    } else if (key == "ArrowUp") {
        params["code"] = "ArrowUp";
        params["windowsVirtualKeyCode"] = 38;
    } else if (key == "ArrowLeft") {
        params["code"] = "ArrowLeft";
        params["windowsVirtualKeyCode"] = 37;
    } else if (key == "ArrowRight") {
        params["code"] = "ArrowRight";
        params["windowsVirtualKeyCode"] = 39;
    } else if (key.size() == 1) {
        params["text"] = key;
    }

    params["type"] = "keyDown";
    send_command("Input.dispatchKeyEvent", params);
    params["type"] = "keyUp";
    send_command("Input.dispatchKeyEvent", params);
    return true;
}

std::vector<std::string> CdpBackend::get_image_urls() {
    auto resp = send_command(
        "Runtime.evaluate",
        {{"expression",
          "JSON.stringify(Array.from(document.images).map(i => i.src))"},
         {"returnByValue", true}});

    std::vector<std::string> urls;
    try {
        auto val = resp.at("result")
                       .at("result")
                       .at("value")
                       .get<std::string>();
        auto arr = nlohmann::json::parse(val);
        for (const auto& u : arr) {
            urls.push_back(u.get<std::string>());
        }
    } catch (...) {
    }
    return urls;
}

ConsoleResult CdpBackend::evaluate_js(const std::string& expression) {
    auto resp = send_command(
        "Runtime.evaluate",
        {{"expression", expression}, {"returnByValue", true}});

    ConsoleResult result;
    try {
        auto& r = resp.at("result").at("result");
        if (r.contains("value")) {
            auto& val = r["value"];
            if (val.is_string()) {
                result.value = val.get<std::string>();
            } else {
                result.value = val.dump();
            }
        } else if (r.contains("description")) {
            result.value = r["description"].get<std::string>();
        }
        if (r.value("subtype", "") == "error" ||
            resp.at("result").contains("exceptionDetails")) {
            result.is_error = true;
        }
    } catch (...) {
        result.value = "CDP evaluation failed";
        result.is_error = true;
    }
    return result;
}

std::string CdpBackend::screenshot_base64() {
    auto resp = send_command("Page.captureScreenshot", {{"format", "png"}});
    try {
        return resp.at("result").at("data").get<std::string>();
    } catch (...) {
        return {};
    }
}

// -- Factory ------------------------------------------------------------------

std::unique_ptr<BrowserBackend> make_cdp_backend(CdpConfig config) {
    auto backend = std::make_unique<CdpBackend>(std::move(config));
    if (!backend->launch()) return nullptr;
    return backend;
}

}  // namespace hermes::tools
