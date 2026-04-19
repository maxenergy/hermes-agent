// Abstract LLM client interface with injectable HTTP transport.
#pragma once

#include "hermes/llm/message.hpp"
#include "hermes/llm/prompt_cache.hpp"
#include "hermes/llm/usage.hpp"

#include <nlohmann/json.hpp>

#include <deque>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace hermes::llm {

struct ToolSchema {
    std::string name;
    std::string description;
    nlohmann::json parameters;  // JSON Schema
};

struct CompletionRequest {
    std::string model;
    std::vector<Message> messages;
    std::vector<ToolSchema> tools;
    std::optional<double> temperature;
    std::optional<int> max_tokens;
    std::optional<int> reasoning_effort;  // 0/1/2/3
    bool stream = false;
    PromptCacheOptions cache;
    nlohmann::json extra;  // provider-specific knobs
};

struct CompletionResponse {
    Message assistant_message;
    CanonicalUsage usage;
    std::string finish_reason;  // stop|tool_calls|length|content_filter
    nlohmann::json raw;         // original provider body
};

// Thrown on non-2xx responses.
struct ApiError : public std::runtime_error {
    int status;
    std::string body;
    std::string provider;
    ApiError(int status_, std::string body_, std::string provider_)
        : std::runtime_error("LLM API error (" + provider_ + " status " +
                             std::to_string(status_) + ")"),
          status(status_),
          body(std::move(body_)),
          provider(std::move(provider_)) {}
};

class LlmClient {
public:
    virtual ~LlmClient() = default;
    virtual CompletionResponse complete(const CompletionRequest& req) = 0;
    virtual std::string provider_name() const = 0;
};

// ── HTTP transport abstraction ──────────────────────────────────────────

class HttpTransport {
public:
    struct Response {
        int status_code = 0;
        std::string body;
        std::unordered_map<std::string, std::string> headers;
    };
    virtual ~HttpTransport() = default;
    virtual Response post_json(
        const std::string& url,
        const std::unordered_map<std::string, std::string>& headers,
        const std::string& body) = 0;

    /// HTTP GET.  Default delegates to post_json with an empty body for
    /// back-compat; the real CurlTransport overrides with a proper GET.
    virtual Response get(
        const std::string& url,
        const std::unordered_map<std::string, std::string>& headers) {
        return post_json(url, headers, "");
    }

    /// Stream a POST request.  Calls callback for each chunk received.
    using StreamCallback = std::function<void(const std::string& chunk)>;
    virtual void post_json_stream(
        const std::string& url,
        const std::unordered_map<std::string, std::string>& headers,
        const std::string& body,
        StreamCallback on_chunk) {
        // Default: non-streaming fallback — deliver the whole body as one chunk.
        auto resp = post_json(url, headers, body);
        on_chunk(resp.body);
    }
};

// Real HTTP transport wrapping libcurl/cpr.  If neither is available at
// configure time, throws std::runtime_error.
std::unique_ptr<HttpTransport> make_curl_transport();

// Lazily-created global transport instance.  Returns nullptr when no HTTP
// backend was compiled in.
HttpTransport* get_default_transport();

// ── TCP keepalive configuration ───────────────────────────────────────
//
// Provider HTTP connections enable socket-level keepalives so dead peers
// are detected within ~60s instead of the process hanging until the 120s
// read timeout — see #10324 (Python port 8c478983) for the underlying
// failure mode (CLOSE-WAIT sockets never surfacing to epoll).
//
// The defaults match the Python port: probe after 30s idle, retry every
// 10s, give up after 3 attempts.  Linux uses TCP_KEEPIDLE, macOS falls
// back to TCP_KEEPALIVE (same semantics, different symbol).  Windows
// support is a no-op — the relevant SIO_KEEPALIVE_VALS IOCTL isn't
// reachable from the curl socket hook.
struct TcpKeepaliveSettings {
    int idle_seconds = 30;     // TCP_KEEPIDLE / TCP_KEEPALIVE
    int interval_seconds = 10; // TCP_KEEPINTVL
    int probe_count = 3;       // TCP_KEEPCNT
};

// Observability hook populated by CurlTransport's sockopt callback right
// before every connect.  Tests use this to assert the options really
// landed on the underlying fd (cannot ``getsockopt`` curl's private
// socket after the fact because it's destroyed by the time control
// returns).
struct LastCurlSocketOptions {
    bool populated = false;
    int so_keepalive = -1;
    int tcp_keepidle = -1;
    int tcp_keepintvl = -1;
    int tcp_keepcnt = -1;
};

// Returns a thread-safe snapshot of the most recent sockopt observation.
LastCurlSocketOptions last_curl_socket_options();

// Clears the observation; tests call this between requests.
void reset_last_curl_socket_options();

// Deterministic fake transport for unit tests.
class FakeHttpTransport : public HttpTransport {
public:
    struct Request {
        std::string url;
        std::unordered_map<std::string, std::string> headers;
        std::string body;
    };

    void enqueue_response(Response r);
    /// Enqueue a stream response — the body is delivered line-by-line to the
    /// StreamCallback when post_json_stream is called.
    void enqueue_stream_response(std::string sse_body);
    Response post_json(
        const std::string& url,
        const std::unordered_map<std::string, std::string>& headers,
        const std::string& body) override;
    void post_json_stream(
        const std::string& url,
        const std::unordered_map<std::string, std::string>& headers,
        const std::string& body,
        StreamCallback on_chunk) override;

    const std::vector<Request>& requests() const { return requests_; }
    bool empty() const { return queue_.empty(); }

private:
    std::deque<Response> queue_;
    std::deque<std::string> stream_queue_;
    std::vector<Request> requests_;
};

}  // namespace hermes::llm
