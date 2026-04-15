#include "hermes/tools/browser_helpers.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <random>
#include <sstream>
#include <system_error>

namespace hermes::tools::browser {

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {

// Lowercase helper (ASCII).
std::string to_lower(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        out.push_back(
            static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    return out;
}

std::string_view trim(std::string_view s) {
    std::size_t b = 0, e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    return s.substr(b, e - b);
}

bool starts_with(std::string_view s, std::string_view p) {
    return s.size() >= p.size() && s.compare(0, p.size(), p) == 0;
}

bool istarts_with(std::string_view s, std::string_view p) {
    if (s.size() < p.size()) return false;
    for (std::size_t i = 0; i < p.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(s[i])) !=
            std::tolower(static_cast<unsigned char>(p[i]))) {
            return false;
        }
    }
    return true;
}

}  // namespace

// ── Selector / ref parsing ────────────────────────────────────────────

ParsedSelector parse_selector(std::string_view s) {
    ParsedSelector out;
    auto t = trim(s);
    if (t.empty()) return out;

    if (t.front() == '@') {
        out.kind = SelectorKind::Ref;
        out.value = std::string(t.substr(1));
        return out;
    }
    if (istarts_with(t, "css=")) {
        out.kind = SelectorKind::Css;
        out.value = std::string(t.substr(4));
        return out;
    }
    if (istarts_with(t, "xpath=")) {
        out.kind = SelectorKind::Xpath;
        out.value = std::string(t.substr(6));
        return out;
    }
    if (istarts_with(t, "text=")) {
        out.kind = SelectorKind::Text;
        out.value = std::string(t.substr(5));
        return out;
    }
    if (istarts_with(t, "role=")) {
        out.kind = SelectorKind::Role;
        auto tail = t.substr(5);
        // role=button[name='Submit']
        auto lb = tail.find('[');
        if (lb == std::string_view::npos) {
            out.value = std::string(tail);
        } else {
            out.value = std::string(tail.substr(0, lb));
            auto rest = tail.substr(lb);
            auto npos = rest.find("name=");
            if (npos != std::string_view::npos) {
                auto after = rest.substr(npos + 5);
                if (!after.empty() && (after.front() == '\'' || after.front() == '"')) {
                    char q = after.front();
                    after.remove_prefix(1);
                    auto end = after.find(q);
                    if (end != std::string_view::npos) {
                        out.role_name = std::string(after.substr(0, end));
                    }
                }
            }
        }
        return out;
    }
    // Bare string → default classification.
    if (t.find("//") == 0 || t.find("(/") == 0) {
        out.kind = SelectorKind::Xpath;
        out.value = std::string(t);
        return out;
    }
    // Matches @xyz? No — we consumed @ above.
    // Otherwise CSS.
    out.kind = SelectorKind::Css;
    out.value = std::string(t);
    return out;
}

std::optional<std::string> extract_ref_id(std::string_view s) {
    auto t = trim(s);
    if (t.size() < 2 || t.front() != '@') return std::nullopt;
    auto rest = t.substr(1);
    for (char c : rest) {
        if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_' ||
              c == '-')) {
            return std::nullopt;
        }
    }
    return std::string(rest);
}

std::string normalize_key(std::string_view key) {
    auto t = trim(key);
    if (t.empty()) return "";

    // Split on '+' while preserving the plus sign itself at the end.
    std::vector<std::string> parts;
    std::string cur;
    for (std::size_t i = 0; i < t.size(); ++i) {
        char c = t[i];
        if (c == '+' && !cur.empty() && i + 1 < t.size()) {
            parts.push_back(cur);
            cur.clear();
        } else {
            cur.push_back(c);
        }
    }
    if (!cur.empty()) parts.push_back(cur);

    static const std::unordered_map<std::string, std::string> single = {
        {"enter", "Enter"},   {"return", "Enter"}, {"\n", "Enter"},
        {"esc", "Escape"},    {"escape", "Escape"},
        {"tab", "Tab"},       {"space", "Space"},  {" ", "Space"},
        {"up", "ArrowUp"},    {"down", "ArrowDown"},
        {"left", "ArrowLeft"},{"right", "ArrowRight"},
        {"backspace", "Backspace"}, {"bs", "Backspace"},
        {"del", "Delete"},    {"delete", "Delete"},
        {"home", "Home"},     {"end", "End"},
        {"pageup", "PageUp"}, {"pagedown", "PageDown"},
        {"ctrl", "Control"},  {"control", "Control"},
        {"shift", "Shift"},   {"alt", "Alt"}, {"meta", "Meta"},
        {"cmd", "Meta"},
    };
    static const std::array<std::string, 12> fn_keys = {
        "f1","f2","f3","f4","f5","f6","f7","f8","f9","f10","f11","f12"};

    std::string result;
    for (std::size_t i = 0; i < parts.size(); ++i) {
        auto lower = to_lower(parts[i]);
        std::string mapped;
        auto it = single.find(lower);
        if (it != single.end()) {
            mapped = it->second;
        } else if (std::find(fn_keys.begin(), fn_keys.end(), lower) !=
                   fn_keys.end()) {
            mapped = "F" + lower.substr(1);
        } else if (parts[i].size() == 1) {
            // Single printable character: uppercase when a modifier precedes it.
            if (i > 0) {
                mapped.push_back(static_cast<char>(
                    std::toupper(static_cast<unsigned char>(parts[i][0]))));
            } else {
                mapped = parts[i];
            }
        } else {
            mapped = parts[i];
        }
        if (!result.empty()) result.push_back('+');
        result += mapped;
    }
    return result;
}

// ── Snapshot truncation ───────────────────────────────────────────────

std::string truncate_snapshot(const std::string& text, std::size_t max_chars) {
    if (text.size() <= max_chars) return text;
    std::size_t cut = max_chars;
    // Try not to split mid-line.
    auto nl = text.rfind('\n', cut);
    if (nl != std::string::npos && nl + 1 >= cut / 2) cut = nl;
    std::ostringstream os;
    os << text.substr(0, cut) << "\n...[TRUNCATED "
       << (text.size() - cut) << " chars]";
    return os.str();
}

std::vector<SnapshotElement> compact_interactive(
    const std::vector<SnapshotElement>& in) {
    std::vector<SnapshotElement> out;
    out.reserve(in.size());
    for (const auto& e : in) {
        if (!e.visible) continue;
        if (e.width <= 0 || e.height <= 0) continue;
        static const std::array<std::string, 12> interactive_tags = {
            "a","button","input","select","textarea","label","summary",
            "details","option","li","menuitem","form"};
        static const std::array<std::string, 10> interactive_roles = {
            "button","link","textbox","checkbox","radio","combobox",
            "menuitem","option","tab","switch"};
        auto tag_lower = to_lower(e.tag);
        auto role_lower = to_lower(e.role);
        bool tag_i = std::find(interactive_tags.begin(),
                               interactive_tags.end(), tag_lower) !=
                     interactive_tags.end();
        bool role_i = std::find(interactive_roles.begin(),
                                interactive_roles.end(), role_lower) !=
                      interactive_roles.end();
        if (!tag_i && !role_i && e.text.empty()) continue;
        out.push_back(e);
    }
    return out;
}

// ── CDP URL normalization ─────────────────────────────────────────────

CdpClassifyResult classify_cdp_url(std::string_view cdp_url) {
    CdpClassifyResult r;
    auto raw = trim(cdp_url);
    r.raw = std::string(raw);
    if (raw.empty()) {
        r.kind = CdpUrlKind::Empty;
        return r;
    }
    auto lower = to_lower(raw);
    if (lower.find("/devtools/browser/") != std::string::npos) {
        r.kind = CdpUrlKind::WebsocketDirect;
        return r;
    }
    if (istarts_with(raw, "ws://") || istarts_with(raw, "wss://")) {
        // ws://host:port → discovery via http(s)://host:port/json/version
        auto scheme_end = raw.find("://");
        auto after = raw.substr(scheme_end + 3);
        auto slash = after.find('/');
        if (slash == std::string_view::npos) {
            r.kind = CdpUrlKind::WebsocketBare;
            std::string host(after);
            r.version_url =
                (istarts_with(raw, "ws://") ? "http://" : "https://") +
                host + "/json/version";
            return r;
        }
        r.kind = CdpUrlKind::WebsocketDirect;
        return r;
    }
    if (istarts_with(raw, "http://") || istarts_with(raw, "https://")) {
        if (lower.size() >= 13 &&
            lower.compare(lower.size() - 13, 13, "/json/version") == 0) {
            r.kind = CdpUrlKind::HttpVersion;
            r.version_url = r.raw;
            return r;
        }
        r.kind = CdpUrlKind::HttpRoot;
        std::string v = r.raw;
        while (!v.empty() && v.back() == '/') v.pop_back();
        v += "/json/version";
        r.version_url = v;
        return r;
    }
    r.kind = CdpUrlKind::Unknown;
    return r;
}

std::string extract_ws_from_version_json(const std::string& json_text) {
    try {
        auto j = json::parse(json_text);
        if (!j.is_object()) return "";
        auto it = j.find("webSocketDebuggerUrl");
        if (it == j.end()) return "";
        if (!it->is_string()) return "";
        auto s = it->get<std::string>();
        auto trimmed = trim(s);
        return std::string(trimmed);
    } catch (...) {
        return "";
    }
}

std::string make_session_name(bool cdp_override) {
    static thread_local std::mt19937_64 rng(std::random_device{}());
    static const char hex[] = "0123456789abcdef";
    std::string out = cdp_override ? "cdp_" : "h_";
    for (int i = 0; i < 10; ++i) {
        out.push_back(hex[rng() & 0xF]);
    }
    return out;
}

// ── Cookie jar ────────────────────────────────────────────────────────

namespace {
bool domain_matches(const std::string& host, const std::string& cookie_domain) {
    // RFC6265 §5.1.3: either exact match or host ends with "." + domain.
    std::string cd = cookie_domain;
    if (!cd.empty() && cd.front() == '.') cd.erase(cd.begin());
    if (host == cd) return true;
    if (host.size() > cd.size() + 1 &&
        host.compare(host.size() - cd.size(), cd.size(), cd) == 0 &&
        host[host.size() - cd.size() - 1] == '.') {
        return true;
    }
    return false;
}
bool path_matches(const std::string& req, const std::string& cookie_path) {
    if (cookie_path.empty() || cookie_path == "/") return true;
    if (req == cookie_path) return true;
    if (starts_with(req, cookie_path)) {
        if (cookie_path.back() == '/' || req[cookie_path.size()] == '/') {
            return true;
        }
    }
    return false;
}
}  // namespace

void CookieJar::set(const Cookie& c) {
    std::lock_guard<std::mutex> lk(mu_);
    for (auto& existing : cookies_) {
        if (existing.name == c.name && existing.domain == c.domain &&
            existing.path == c.path) {
            existing = c;
            return;
        }
    }
    cookies_.push_back(c);
}

void CookieJar::remove(const std::string& domain, const std::string& name) {
    std::lock_guard<std::mutex> lk(mu_);
    cookies_.erase(
        std::remove_if(cookies_.begin(), cookies_.end(),
                       [&](const Cookie& c) {
                           return c.domain == domain && c.name == name;
                       }),
        cookies_.end());
}

void CookieJar::clear() {
    std::lock_guard<std::mutex> lk(mu_);
    cookies_.clear();
}

std::vector<Cookie> CookieJar::match(std::string_view host,
                                     std::string_view path,
                                     bool https) const {
    std::lock_guard<std::mutex> lk(mu_);
    std::vector<Cookie> out;
    std::string h(host), p(path);
    for (const auto& c : cookies_) {
        if (!domain_matches(h, c.domain)) continue;
        if (!path_matches(p, c.path)) continue;
        if (c.secure && !https) continue;
        out.push_back(c);
    }
    return out;
}

std::string CookieJar::to_json() const {
    std::lock_guard<std::mutex> lk(mu_);
    json arr = json::array();
    for (const auto& c : cookies_) {
        arr.push_back({
            {"name", c.name}, {"value", c.value},
            {"domain", c.domain}, {"path", c.path},
            {"httpOnly", c.http_only}, {"secure", c.secure},
            {"expires", c.expires_unix}, {"sameSite", c.same_site}});
    }
    return arr.dump();
}

bool CookieJar::from_json(const std::string& text) {
    try {
        auto j = json::parse(text);
        if (!j.is_array()) return false;
        std::lock_guard<std::mutex> lk(mu_);
        cookies_.clear();
        for (const auto& e : j) {
            Cookie c;
            c.name = e.value("name", "");
            c.value = e.value("value", "");
            c.domain = e.value("domain", "");
            c.path = e.value("path", "/");
            c.http_only = e.value("httpOnly", false);
            c.secure = e.value("secure", false);
            c.expires_unix = e.value("expires", static_cast<int64_t>(0));
            c.same_site = e.value("sameSite", "Lax");
            cookies_.push_back(c);
        }
        return true;
    } catch (...) {
        return false;
    }
}

std::size_t CookieJar::size() const {
    std::lock_guard<std::mutex> lk(mu_);
    return cookies_.size();
}

// ── Console capture ───────────────────────────────────────────────────

std::string console_level_to_string(ConsoleLevel l) {
    switch (l) {
        case ConsoleLevel::Log: return "log";
        case ConsoleLevel::Info: return "info";
        case ConsoleLevel::Warn: return "warn";
        case ConsoleLevel::Error: return "error";
        case ConsoleLevel::Debug: return "debug";
    }
    return "log";
}

ConsoleLevel console_level_from_string(std::string_view s) {
    auto t = to_lower(s);
    if (t == "info") return ConsoleLevel::Info;
    if (t == "warn" || t == "warning") return ConsoleLevel::Warn;
    if (t == "error" || t == "err") return ConsoleLevel::Error;
    if (t == "debug") return ConsoleLevel::Debug;
    return ConsoleLevel::Log;
}

ConsoleBuffer::ConsoleBuffer(std::size_t cap) : cap_(cap) {}

void ConsoleBuffer::push(ConsoleMessage m) {
    std::lock_guard<std::mutex> lk(mu_);
    buf_.push_back(std::move(m));
    while (buf_.size() > cap_) buf_.pop_front();
}

void ConsoleBuffer::clear() {
    std::lock_guard<std::mutex> lk(mu_);
    buf_.clear();
}

std::size_t ConsoleBuffer::size() const {
    std::lock_guard<std::mutex> lk(mu_);
    return buf_.size();
}

std::vector<ConsoleMessage> ConsoleBuffer::drain() {
    std::lock_guard<std::mutex> lk(mu_);
    std::vector<ConsoleMessage> out(buf_.begin(), buf_.end());
    buf_.clear();
    return out;
}

std::vector<ConsoleMessage> ConsoleBuffer::peek() const {
    std::lock_guard<std::mutex> lk(mu_);
    return {buf_.begin(), buf_.end()};
}

std::string ConsoleBuffer::format(const std::vector<std::string>& levels) const {
    std::lock_guard<std::mutex> lk(mu_);
    std::ostringstream os;
    for (const auto& m : buf_) {
        if (!levels.empty()) {
            auto ls = console_level_to_string(m.level);
            bool keep = false;
            for (const auto& lv : levels) {
                if (to_lower(lv) == ls) {
                    keep = true;
                    break;
                }
            }
            if (!keep) continue;
        }
        os << "[" << console_level_to_string(m.level) << "] " << m.text;
        if (!m.source.empty()) {
            os << "  (" << m.source << ":" << m.line << ":" << m.column << ")";
        }
        os << '\n';
    }
    return os.str();
}

// ── Network request log ───────────────────────────────────────────────

NetworkLog::NetworkLog(std::size_t cap) : cap_(cap) {}

void NetworkLog::on_request(NetRequest r) {
    std::lock_guard<std::mutex> lk(mu_);
    log_.push_back(std::move(r));
    while (log_.size() > cap_) log_.pop_front();
}

void NetworkLog::on_response(const std::string& id, int status, int64_t bs,
                             double ms) {
    std::lock_guard<std::mutex> lk(mu_);
    for (auto& r : log_) {
        if (r.id == id) {
            r.status = status;
            r.body_size = bs;
            r.duration_ms = ms;
            return;
        }
    }
}

void NetworkLog::clear() {
    std::lock_guard<std::mutex> lk(mu_);
    log_.clear();
}

std::size_t NetworkLog::size() const {
    std::lock_guard<std::mutex> lk(mu_);
    return log_.size();
}

std::vector<NetRequest> NetworkLog::filter(std::string_view url_substr,
                                           std::string_view rtype) const {
    std::lock_guard<std::mutex> lk(mu_);
    std::vector<NetRequest> out;
    for (const auto& r : log_) {
        if (!url_substr.empty() &&
            r.url.find(url_substr) == std::string::npos) {
            continue;
        }
        if (!rtype.empty() && r.resource_type != rtype) continue;
        out.push_back(r);
    }
    return out;
}

// ── Download handling ─────────────────────────────────────────────────

DownloadDestResult validate_download_destination(std::string_view requested,
                                                 std::string_view root) {
    DownloadDestResult r;
    if (requested.empty()) {
        r.error = "requested path is empty";
        return r;
    }
    if (root.empty()) {
        r.error = "allowed root is empty";
        return r;
    }
    fs::path req(std::string{requested});
    fs::path rp(std::string{root});
    if (req.filename().empty()) {
        r.error = "requested path has no filename component";
        return r;
    }
    auto fn = req.filename().string();
    if (fn == "." || fn == "..") {
        r.error = "filename contains path traversal";
        return r;
    }
    if (fn.find('/') != std::string::npos ||
        fn.find('\\') != std::string::npos) {
        r.error = "filename contains path separator";
        return r;
    }
    std::error_code ec;
    fs::path abs_req = fs::weakly_canonical(
        req.is_absolute() ? req : rp / req, ec);
    if (ec) {
        r.error = "cannot canonicalise requested path";
        return r;
    }
    fs::path abs_root = fs::weakly_canonical(rp, ec);
    if (ec) {
        r.error = "cannot canonicalise allowed root";
        return r;
    }
    auto rs = abs_root.string();
    auto ps = abs_req.string();
    if (!rs.empty() && rs.back() != '/') rs.push_back('/');
    if (!starts_with(ps + '/', rs)) {
        r.error = "requested path escapes allowed root: " + abs_req.string();
        return r;
    }
    r.normalized_path = abs_req.string();
    return r;
}

std::string sniff_mime(std::string_view buf) {
    auto b = [&](std::size_t i) -> unsigned char {
        return i < buf.size() ? static_cast<unsigned char>(buf[i]) : 0;
    };
    if (buf.size() >= 4 && b(0) == '%' && b(1) == 'P' && b(2) == 'D' &&
        b(3) == 'F') {
        return "application/pdf";
    }
    if (buf.size() >= 8 && b(0) == 0x89 && b(1) == 'P' && b(2) == 'N' &&
        b(3) == 'G') {
        return "image/png";
    }
    if (buf.size() >= 3 && b(0) == 0xFF && b(1) == 0xD8 && b(2) == 0xFF) {
        return "image/jpeg";
    }
    if (buf.size() >= 6 && b(0) == 'G' && b(1) == 'I' && b(2) == 'F' &&
        b(3) == '8') {
        return "image/gif";
    }
    if (buf.size() >= 4 && b(0) == 'P' && b(1) == 'K' && b(2) == 0x03 &&
        b(3) == 0x04) {
        return "application/zip";
    }
    if (buf.size() >= 4 && b(0) == 'R' && b(1) == 'I' && b(2) == 'F' &&
        b(3) == 'F') {
        return "audio/wav";
    }
    // HTML heuristic.
    auto lower = to_lower(buf.substr(0, std::min<std::size_t>(512, buf.size())));
    if (lower.find("<!doctype html") != std::string::npos ||
        lower.find("<html") != std::string::npos) {
        return "text/html";
    }
    if (lower.find("<?xml") == 0) return "application/xml";
    return "";
}

// ── Dialog handling ───────────────────────────────────────────────────

DialogDecision decide_dialog(std::string_view kind, const DialogPolicy& p) {
    DialogDecision d;
    auto k = to_lower(kind);
    auto act = [](DialogAction a) {
        return a == DialogAction::Accept ? "accept" : "dismiss";
    };
    if (k == "alert") {
        d.handled = p.alert_auto;
        d.action = act(p.alert_action);
    } else if (k == "confirm") {
        d.handled = p.confirm_auto;
        d.action = act(p.confirm_action);
    } else if (k == "prompt") {
        d.handled = p.prompt_auto;
        d.action = act(p.prompt_action);
        if (p.prompt_action == DialogAction::Accept) d.text = p.prompt_text;
    } else if (k == "beforeunload") {
        d.handled = p.beforeunload_auto;
        d.action = act(p.beforeunload_action);
    }
    return d;
}

// ── File chooser shim ─────────────────────────────────────────────────

std::string validate_file_chooser_paths(const std::vector<std::string>& paths,
                                        std::size_t max_bytes) {
    if (paths.empty()) return "no files supplied";
    for (const auto& p : paths) {
        fs::path fp(p);
        std::error_code ec;
        if (!fs::exists(fp, ec)) return "file does not exist: " + p;
        if (!fs::is_regular_file(fp, ec)) return "not a regular file: " + p;
        auto sz = fs::file_size(fp, ec);
        if (ec) return "cannot stat file: " + p;
        if (sz > max_bytes) {
            return "file too large: " + p + " (" + std::to_string(sz) + " > " +
                   std::to_string(max_bytes) + ")";
        }
    }
    return "";
}

// ── Emulation ─────────────────────────────────────────────────────────

DeviceProfile device_profile(std::string_view name) {
    static const std::unordered_map<std::string, DeviceProfile> catalog = {
        {"pixel 5",
         {"Pixel 5", 393, 851, 2.75, true, true,
          "Mozilla/5.0 (Linux; Android 11; Pixel 5) AppleWebKit/537.36 "
          "(KHTML, like Gecko) Chrome/117.0.0.0 Mobile Safari/537.36"}},
        {"iphone 13",
         {"iPhone 13", 390, 844, 3.0, true, true,
          "Mozilla/5.0 (iPhone; CPU iPhone OS 15_0 like Mac OS X) "
          "AppleWebKit/605.1.15 (KHTML, like Gecko) Version/15.0 "
          "Mobile/15E148 Safari/604.1"}},
        {"ipad mini",
         {"iPad Mini", 768, 1024, 2.0, true, true,
          "Mozilla/5.0 (iPad; CPU OS 15_0 like Mac OS X) AppleWebKit/"
          "605.1.15 (KHTML, like Gecko) Version/15.0 Mobile/15E148 "
          "Safari/604.1"}},
        {"desktop",
         {"Desktop", 1280, 800, 1.0, false, false, ""}},
        {"desktop hidpi",
         {"Desktop HiDPI", 1920, 1080, 2.0, false, false, ""}},
    };
    auto it = catalog.find(to_lower(name));
    if (it == catalog.end()) return {};
    return it->second;
}

std::vector<std::string> device_profile_names() {
    return {"Pixel 5", "iPhone 13", "iPad Mini", "Desktop", "Desktop HiDPI"};
}

std::string emulation_to_cdp_payload_json(const EmulationSettings& s) {
    json j;
    if (s.device) {
        json d;
        d["name"] = s.device->name;
        d["width"] = s.device->width;
        d["height"] = s.device->height;
        d["deviceScaleFactor"] = s.device->device_scale_factor;
        d["mobile"] = s.device->mobile;
        d["hasTouch"] = s.device->has_touch;
        if (!s.device->user_agent.empty()) d["userAgent"] = s.device->user_agent;
        j["device"] = std::move(d);
    }
    j["darkMode"] = s.dark_mode;
    if (!s.locale.empty()) j["locale"] = s.locale;
    if (!s.timezone.empty()) j["timezoneId"] = s.timezone;
    j["reducedMotion"] = s.reduced_motion ? "reduce" : "no-preference";
    return j.dump();
}

// ── Wait condition ────────────────────────────────────────────────────

WaitState parse_wait_state(std::string_view s) {
    auto t = to_lower(s);
    if (t == "attached" || t == "present") return WaitState::Attached;
    if (t == "detached" || t == "removed") return WaitState::Detached;
    if (t == "hidden") return WaitState::Hidden;
    return WaitState::Visible;
}

std::string wait_state_to_string(WaitState s) {
    switch (s) {
        case WaitState::Attached: return "attached";
        case WaitState::Detached: return "detached";
        case WaitState::Visible: return "visible";
        case WaitState::Hidden: return "hidden";
    }
    return "visible";
}

// ── Tabs ──────────────────────────────────────────────────────────────

std::string TabRegistry::open(const std::string& url) {
    std::lock_guard<std::mutex> lk(mu_);
    TabInfo t;
    t.id = "tab-" + std::to_string(next_id_++);
    t.url = url;
    t.active = true;
    for (auto& o : tabs_) o.active = false;
    active_id_ = t.id;
    tabs_.push_back(std::move(t));
    return tabs_.back().id;
}

bool TabRegistry::close(const std::string& id) {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = std::find_if(tabs_.begin(), tabs_.end(),
                           [&](const TabInfo& t) { return t.id == id; });
    if (it == tabs_.end()) return false;
    bool was_active = it->active;
    tabs_.erase(it);
    if (was_active) {
        if (!tabs_.empty()) {
            tabs_.back().active = true;
            active_id_ = tabs_.back().id;
        } else {
            active_id_.clear();
        }
    }
    return true;
}

bool TabRegistry::activate(const std::string& id) {
    std::lock_guard<std::mutex> lk(mu_);
    bool found = false;
    for (auto& t : tabs_) {
        if (t.id == id) {
            t.active = true;
            found = true;
        } else {
            t.active = false;
        }
    }
    if (found) active_id_ = id;
    return found;
}

void TabRegistry::update(const std::string& id, const std::string& url,
                         const std::string& title) {
    std::lock_guard<std::mutex> lk(mu_);
    for (auto& t : tabs_) {
        if (t.id == id) {
            t.url = url;
            t.title = title;
            return;
        }
    }
}

std::vector<TabInfo> TabRegistry::list() const {
    std::lock_guard<std::mutex> lk(mu_);
    return tabs_;
}

std::optional<TabInfo> TabRegistry::active_tab() const {
    std::lock_guard<std::mutex> lk(mu_);
    for (const auto& t : tabs_) if (t.active) return t;
    return std::nullopt;
}

std::size_t TabRegistry::size() const {
    std::lock_guard<std::mutex> lk(mu_);
    return tabs_.size();
}

void TabRegistry::clear() {
    std::lock_guard<std::mutex> lk(mu_);
    tabs_.clear();
    active_id_.clear();
}

// ── Viewport / scroll-into-view maths ─────────────────────────────────

bool rect_in_viewport(const Rect& e, int vw, int vh) {
    return e.x >= 0 && e.y >= 0 && e.x + e.width <= vw && e.y + e.height <= vh;
}

std::pair<double, double> scroll_delta_to_show(const Rect& e, int vw, int vh) {
    double dx = 0, dy = 0;
    if (e.x < 0) dx = e.x;
    else if (e.x + e.width > vw) dx = e.x + e.width - vw;
    if (e.y < 0) dy = e.y;
    else if (e.y + e.height > vh) dy = e.y + e.height - vh;
    return {dx, dy};
}

// ── Redirect handling ─────────────────────────────────────────────────

std::string first_unsafe_redirect(
    const std::vector<std::string>& chain, bool allow_private,
    const std::function<bool(const std::string&)>& is_safe) {
    if (allow_private) return "";
    for (const auto& url : chain) {
        if (!is_safe(url)) return url;
    }
    return "";
}

// ── JS marshaling ─────────────────────────────────────────────────────

std::string marshal_js_value(const std::string& raw) {
    try {
        auto j = json::parse(raw);
        return j.dump();
    } catch (...) {
        return json(raw).dump();
    }
}

}  // namespace hermes::tools::browser
