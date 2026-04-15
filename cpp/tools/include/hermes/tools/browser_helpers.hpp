// Browser helpers — pure utilities ported from tools/browser_tool.py.
//
// These helpers are intentionally backend-agnostic and side-effect free.
// They cover selector / ref parsing, CDP URL normalization, snapshot
// truncation, key normalization, cookie jars, console / network log
// buffers, download destination validation, and download MIME sniffing.
//
// The logic mirrors small self-contained sections of the Python
// tools/browser_tool.py so that the C++ port can be tested without
// spinning up a real Chromium or Playwright instance.
#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace hermes::tools::browser {

// ── Selector / ref parsing ────────────────────────────────────────────

// Kinds of selector accepted by the high-level browser tool surface.
enum class SelectorKind {
    Ref,        // "@e5"
    Css,        // "div.button > span"
    Xpath,      // "//div[@class='x']"
    Text,       // "text=Hello world"
    Role,       // "role=button[name='Submit']"
    Unknown,
};

struct ParsedSelector {
    SelectorKind kind{SelectorKind::Unknown};
    std::string value;       // after kind prefix is stripped
    std::string role_name;   // populated for Role selectors only
};

// Classify a selector string (leading whitespace ignored).
//
// Accepted prefixes (case-insensitive for the prefix itself):
//   "@..."          → Ref
//   "css=..."       → Css
//   "xpath=..."     → Xpath
//   "text=..."      → Text
//   "role=name[...]" → Role
//
// A bare string containing "//" is treated as Xpath; a bare string
// containing "=" or starting with ">" is treated as Css; otherwise it
// defaults to Ref when it matches the @[a-zA-Z0-9_-]+ shape, and Css
// otherwise (matches Playwright's default behaviour).
ParsedSelector parse_selector(std::string_view s);

// Extract the ref ID from a "@eNN" / "@a1" style reference.  Returns
// the portion after "@" on success, or empty on malformed input.
std::optional<std::string> extract_ref_id(std::string_view s);

// Normalise a keyboard key name to the canonical Playwright form:
//   "enter" / "return" / "\n"  → "Enter"
//   "esc"                      → "Escape"
//   "ctrl+a"                   → "Control+A"
// Unknown keys pass through unchanged (but with leading/trailing ws
// trimmed).
std::string normalize_key(std::string_view key);

// ── Snapshot truncation ───────────────────────────────────────────────

// Trim |snapshot_text| so it does not exceed |max_chars| bytes.  The
// returned string preserves the prefix unchanged and appends a
// "...[TRUNCATED N chars]" marker.  When the input already fits, it is
// returned unchanged.
std::string truncate_snapshot(const std::string& snapshot_text,
                              std::size_t max_chars = 8000);

// Filter out zero-area / hidden elements from a snapshot element list.
// Used for the "compact" view returned after navigation.
struct SnapshotElement {
    std::string ref;
    std::string tag;
    std::string text;
    std::string role;
    bool visible{true};
    int width{0};
    int height{0};
};
std::vector<SnapshotElement> compact_interactive(
    const std::vector<SnapshotElement>& in);

// ── CDP URL normalization ─────────────────────────────────────────────

// Classification of a user-supplied CDP endpoint string.
enum class CdpUrlKind {
    Empty,
    WebsocketDirect,    // ws://host:port/devtools/browser/<id>
    WebsocketBare,      // ws://host:port
    HttpVersion,        // http://host:port/json/version
    HttpRoot,           // http://host:port or http://host:port/
    Unknown,
};

struct CdpClassifyResult {
    CdpUrlKind kind{CdpUrlKind::Empty};
    std::string raw;
    std::string version_url;  // non-empty when discovery is required
};

// Classify |cdp_url| and compute the discovery URL that needs to be
// fetched to resolve it to a concrete webSocketDebuggerUrl.  Does not
// perform network I/O — callers plug the result into an HTTP client and
// feed the JSON response back into |extract_ws_from_version_json|.
CdpClassifyResult classify_cdp_url(std::string_view cdp_url);

// Extract the webSocketDebuggerUrl field from /json/version payload.
// Returns empty when the payload is not a JSON object or has no such
// field.  Accepts a raw JSON string.
std::string extract_ws_from_version_json(const std::string& json_text);

// Generate a random session name of the form "h_XXXXXXXXXX" (local
// backend) or "cdp_XXXXXXXXXX" (CDP override).  Uses a thread-local
// PRNG seeded from std::random_device.
std::string make_session_name(bool cdp_override);

// ── Cookie jar ────────────────────────────────────────────────────────

struct Cookie {
    std::string name;
    std::string value;
    std::string domain;
    std::string path{"/"};
    bool http_only{false};
    bool secure{false};
    int64_t expires_unix{0};   // 0 → session cookie
    std::string same_site{"Lax"};  // Lax|Strict|None
};

// Cookie jar with hostname-scoped lookup that mirrors the Netscape
// cookie matching rules Playwright/CDP use.  Thread-safe.
class CookieJar {
public:
    void set(const Cookie& cookie);
    void remove(const std::string& domain, const std::string& name);
    void clear();

    // Return cookies matching |host| + |path| + |https|.  Follows RFC
    // 6265 §5.4 matching (domain suffix + path prefix, Secure filter).
    std::vector<Cookie> match(std::string_view host,
                              std::string_view path,
                              bool https) const;

    // Serialise to/from a CDP-style JSON array.
    std::string to_json() const;
    bool from_json(const std::string& text);

    std::size_t size() const;

private:
    mutable std::mutex mu_;
    std::vector<Cookie> cookies_;
};

// ── Console capture ───────────────────────────────────────────────────

enum class ConsoleLevel { Log, Info, Warn, Error, Debug };

struct ConsoleMessage {
    ConsoleLevel level{ConsoleLevel::Log};
    std::string text;
    std::string source;          // script URL
    int line{0};
    int column{0};
    std::chrono::system_clock::time_point ts;
};

class ConsoleBuffer {
public:
    explicit ConsoleBuffer(std::size_t capacity = 500);
    void push(ConsoleMessage msg);
    void clear();
    std::size_t size() const;
    std::vector<ConsoleMessage> drain();
    std::vector<ConsoleMessage> peek() const;

    // Format with optional level filter.  Levels: "log", "info", etc.
    std::string format(const std::vector<std::string>& levels = {}) const;

private:
    mutable std::mutex mu_;
    std::size_t cap_;
    std::deque<ConsoleMessage> buf_;
};

std::string console_level_to_string(ConsoleLevel lvl);
ConsoleLevel console_level_from_string(std::string_view s);

// ── Network request log ───────────────────────────────────────────────

struct NetRequest {
    std::string id;              // CDP request id
    std::string url;
    std::string method;          // GET / POST / ...
    std::string resource_type;   // document|xhr|fetch|image|...
    int status{0};               // 0 when still pending
    int64_t body_size{0};
    double duration_ms{0.0};
    std::chrono::system_clock::time_point ts;
};

class NetworkLog {
public:
    explicit NetworkLog(std::size_t capacity = 1000);
    void on_request(NetRequest req);
    void on_response(const std::string& id, int status, int64_t body_size,
                     double duration_ms);
    void clear();
    std::size_t size() const;

    // Filter by substring of URL or by resource_type (empty = any).
    std::vector<NetRequest> filter(std::string_view url_substr,
                                   std::string_view resource_type = "") const;

private:
    mutable std::mutex mu_;
    std::size_t cap_;
    std::deque<NetRequest> log_;
};

// ── Download handling ─────────────────────────────────────────────────

// Result of validating a requested download destination.
struct DownloadDestResult {
    std::string normalized_path;  // success only
    std::string error;            // non-empty on failure
};

// Validate that |requested| points into |allowed_root| (after
// canonicalisation) and that the filename is free of path-traversal
// artefacts.  Used to prevent malicious pages from directing downloads
// outside the session cache.
DownloadDestResult validate_download_destination(std::string_view requested,
                                                 std::string_view allowed_root);

// Sniff a MIME type from the first few bytes of |buf|.  Handles the
// common cases (PDF, PNG, JPEG, GIF, ZIP, HTML).  Returns empty when
// no signature matches.
std::string sniff_mime(std::string_view buf);

// ── Dialog handling ───────────────────────────────────────────────────

// Which dialog types the tool auto-handles, and how.
enum class DialogAction { Accept, Dismiss };

struct DialogPolicy {
    bool alert_auto{true};
    DialogAction alert_action{DialogAction::Accept};
    bool confirm_auto{true};
    DialogAction confirm_action{DialogAction::Accept};
    bool prompt_auto{true};
    DialogAction prompt_action{DialogAction::Dismiss};
    std::string prompt_text;  // used when prompt_action == Accept
    bool beforeunload_auto{true};
    DialogAction beforeunload_action{DialogAction::Accept};
};

// Decide how to handle a dialog of |kind| under |policy|.  |kind| is
// one of "alert" | "confirm" | "prompt" | "beforeunload".  Returns
// ("action", "text", "handled") — "handled" is false when policy says
// "do not auto-handle this kind".
struct DialogDecision {
    std::string action;  // "accept" or "dismiss"
    std::string text;    // only non-empty for accepted prompts
    bool handled{false};
};
DialogDecision decide_dialog(std::string_view kind, const DialogPolicy& policy);

// ── File chooser shim ─────────────────────────────────────────────────

// Validate a set of file paths being supplied to a <input type=file>.
// Rejects paths that don't exist, aren't regular files, or whose size
// exceeds |max_bytes_per_file|.  Returns empty error on success.
std::string validate_file_chooser_paths(const std::vector<std::string>& paths,
                                        std::size_t max_bytes_per_file);

// ── Emulation ─────────────────────────────────────────────────────────

struct DeviceProfile {
    std::string name;
    int width{0};
    int height{0};
    double device_scale_factor{1.0};
    bool mobile{false};
    bool has_touch{false};
    std::string user_agent;
};

// Return a baked-in device profile by name (Pixel 5, iPhone 13, iPad
// Mini, Desktop).  Returns empty profile (name=="") when unknown.
DeviceProfile device_profile(std::string_view name);

// List all baked-in device names.
std::vector<std::string> device_profile_names();

struct EmulationSettings {
    std::optional<DeviceProfile> device;
    bool dark_mode{false};
    std::string locale;      // "en-US", "zh-CN", ...
    std::string timezone;    // "America/Los_Angeles", ...
    bool reduced_motion{false};
};

// Build the CDP-style Emulation.* payload dictionary for a given set
// of settings.  Returned as a nlohmann::json string to avoid exposing
// the json type in the public header.
std::string emulation_to_cdp_payload_json(const EmulationSettings& s);

// ── Element wait condition ────────────────────────────────────────────

enum class WaitState { Attached, Detached, Visible, Hidden };

struct WaitCondition {
    std::string selector;
    WaitState state{WaitState::Visible};
    int timeout_ms{30000};
};

WaitState parse_wait_state(std::string_view s);
std::string wait_state_to_string(WaitState s);

// ── Multi-tab / context book-keeping ──────────────────────────────────

struct TabInfo {
    std::string id;
    std::string url;
    std::string title;
    bool active{false};
};

class TabRegistry {
public:
    std::string open(const std::string& url);  // returns new id
    bool close(const std::string& id);
    bool activate(const std::string& id);
    void update(const std::string& id, const std::string& url,
                const std::string& title);
    std::vector<TabInfo> list() const;
    std::optional<TabInfo> active_tab() const;
    std::size_t size() const;
    void clear();

private:
    mutable std::mutex mu_;
    std::vector<TabInfo> tabs_;
    std::string active_id_;
    std::uint64_t next_id_{1};
};

// ── Viewport / scroll-into-view maths ─────────────────────────────────

struct Rect {
    double x{0};
    double y{0};
    double width{0};
    double height{0};
};

// Return true when |element| intersects the rectangle (0,0,w,h).  Used
// by tests and by the "scroll element into view if needed" code path.
bool rect_in_viewport(const Rect& element, int viewport_w, int viewport_h);

// Compute the minimal (dx, dy) scroll that brings |element| fully
// inside the viewport.  Returns (0,0) when already visible.
std::pair<double, double> scroll_delta_to_show(const Rect& element,
                                               int viewport_w,
                                               int viewport_h);

// ── Redirect handling ─────────────────────────────────────────────────

// When following an HTTP redirect chain for navigation, validate each
// target against |allow_private|.  Returns the first target that
// should be blocked, or empty string when the chain is safe.
std::string first_unsafe_redirect(
    const std::vector<std::string>& chain,
    bool allow_private,
    const std::function<bool(const std::string&)>& is_safe);

// ── JS marshaling ─────────────────────────────────────────────────────

// Marshal a C++ string return value from evaluate_js() back into a
// JSON value.  If |raw| parses as JSON, return the parsed value; else
// wrap it as a JSON string.  Returned as text.
std::string marshal_js_value(const std::string& raw);

}  // namespace hermes::tools::browser
