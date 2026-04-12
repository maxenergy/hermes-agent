// Base HTTP transport implementations: CurlTransport + FakeHttpTransport (test double).
#include "hermes/llm/llm_client.hpp"

#include <cstdlib>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <utility>

#if defined(HERMES_LLM_HAS_CURL)
#include <curl/curl.h>
#elif defined(HERMES_LLM_HAS_CPR)
#include <cpr/cpr.h>
#endif

namespace hermes::llm {

// ── FakeHttpTransport ──────────────────────────────────────────────────

void FakeHttpTransport::enqueue_response(Response r) {
    queue_.push_back(std::move(r));
}

HttpTransport::Response FakeHttpTransport::post_json(
    const std::string& url,
    const std::unordered_map<std::string, std::string>& headers,
    const std::string& body) {
    requests_.push_back(Request{url, headers, body});
    if (queue_.empty()) {
        throw std::runtime_error("FakeHttpTransport: no response enqueued");
    }
    Response r = std::move(queue_.front());
    queue_.pop_front();
    return r;
}

void FakeHttpTransport::enqueue_stream_response(std::string sse_body) {
    stream_queue_.push_back(std::move(sse_body));
}

void FakeHttpTransport::post_json_stream(
    const std::string& url,
    const std::unordered_map<std::string, std::string>& headers,
    const std::string& body,
    StreamCallback on_chunk) {
    requests_.push_back(Request{url, headers, body});
    if (stream_queue_.empty()) {
        throw std::runtime_error("FakeHttpTransport: no stream response enqueued");
    }
    std::string sse = std::move(stream_queue_.front());
    stream_queue_.pop_front();
    // Deliver each line as a separate chunk, mimicking SSE line delivery.
    std::istringstream ss(sse);
    std::string line;
    while (std::getline(ss, line)) {
        on_chunk(line + "\n");
    }
}

// ── Real HTTP transport ────────────────────────────────────────────────

#if defined(HERMES_LLM_HAS_CURL)

namespace {

/// libcurl write callback — appends data to a std::string.
size_t write_callback(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* buf = static_cast<std::string*>(userdata);
    const size_t total = size * nmemb;
    buf->append(ptr, total);
    return total;
}

/// libcurl header callback — parses "Key: Value" lines into a map.
size_t header_callback(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* hdrs =
        static_cast<std::unordered_map<std::string, std::string>*>(userdata);
    const size_t total = size * nmemb;
    std::string line(ptr, total);
    // Strip trailing \r\n.
    while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) {
        line.pop_back();
    }
    const auto colon = line.find(':');
    if (colon != std::string::npos) {
        std::string key = line.substr(0, colon);
        std::string value = line.substr(colon + 1);
        // Trim leading whitespace from value.
        const auto start = value.find_first_not_of(' ');
        if (start != std::string::npos) {
            value = value.substr(start);
        }
        (*hdrs)[std::move(key)] = std::move(value);
    }
    return total;
}

/// Streaming write callback — accumulates partial lines and fires callback
/// for each complete line.
struct StreamCtx {
    HttpTransport::StreamCallback* on_chunk;
    std::string line_buffer;
};

size_t stream_write_callback(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* ctx = static_cast<StreamCtx*>(userdata);
    const size_t total = size * nmemb;
    ctx->line_buffer.append(ptr, total);
    // Deliver complete lines to the callback.
    std::string::size_type pos;
    while ((pos = ctx->line_buffer.find('\n')) != std::string::npos) {
        std::string line = ctx->line_buffer.substr(0, pos + 1);
        ctx->line_buffer.erase(0, pos + 1);
        (*ctx->on_chunk)(line);
    }
    return total;
}

class CurlTransport : public HttpTransport {
public:
    CurlTransport() { curl_global_init(CURL_GLOBAL_DEFAULT); }
    ~CurlTransport() override { curl_global_cleanup(); }

    Response post_json(
        const std::string& url,
        const std::unordered_map<std::string, std::string>& headers,
        const std::string& body) override {
        return perform(url, headers, &body, /*is_post=*/true);
    }

    Response get(
        const std::string& url,
        const std::unordered_map<std::string, std::string>& headers) override {
        return perform(url, headers, nullptr, /*is_post=*/false);
    }

    void post_json_stream(
        const std::string& url,
        const std::unordered_map<std::string, std::string>& headers,
        const std::string& body,
        StreamCallback on_chunk) override {
        CURL* curl = curl_easy_init();
        if (!curl) {
            throw std::runtime_error("curl_easy_init() failed");
        }

        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 300L);  // longer for streaming
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);

        // Proxy support.
        const char* proxy = std::getenv("HTTPS_PROXY");
        if (!proxy) proxy = std::getenv("https_proxy");
        if (!proxy) proxy = std::getenv("HTTP_PROXY");
        if (!proxy) proxy = std::getenv("http_proxy");
        if (proxy && proxy[0] != '\0') {
            curl_easy_setopt(curl, CURLOPT_PROXY, proxy);
        }

        struct curl_slist* header_list = nullptr;
        bool has_content_type = false;
        for (const auto& kv : headers) {
            std::string h = kv.first + ": " + kv.second;
            header_list = curl_slist_append(header_list, h.c_str());
            if (kv.first == "Content-Type") has_content_type = true;
        }
        if (!has_content_type) {
            header_list = curl_slist_append(
                header_list, "Content-Type: application/json");
        }
        if (header_list) {
            curl_easy_setopt(curl, CURLOPT_HTTPHEADER, header_list);
        }

        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE,
                         static_cast<long>(body.size()));

        StreamCtx ctx{&on_chunk, {}};
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, stream_write_callback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &ctx);

        CURLcode res = curl_easy_perform(curl);
        // Flush any remaining data in the line buffer.
        if (!ctx.line_buffer.empty()) {
            on_chunk(ctx.line_buffer);
        }

        curl_slist_free_all(header_list);
        curl_easy_cleanup(curl);

        if (res != CURLE_OK) {
            throw std::runtime_error(
                std::string("curl streaming request failed: ") +
                curl_easy_strerror(res));
        }
    }

private:
    Response perform(
        const std::string& url,
        const std::unordered_map<std::string, std::string>& headers,
        const std::string* body,
        bool is_post) {
        CURL* curl = curl_easy_init();
        if (!curl) {
            throw std::runtime_error("curl_easy_init() failed");
        }

        Response out;
        std::string resp_body;

        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 120L);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);

        // Proxy support via environment.
        const char* proxy = std::getenv("HTTPS_PROXY");
        if (!proxy) proxy = std::getenv("https_proxy");
        if (!proxy) proxy = std::getenv("HTTP_PROXY");
        if (!proxy) proxy = std::getenv("http_proxy");
        if (proxy && proxy[0] != '\0') {
            curl_easy_setopt(curl, CURLOPT_PROXY, proxy);
        }

        // Headers.
        struct curl_slist* header_list = nullptr;
        bool has_content_type = false;
        for (const auto& kv : headers) {
            std::string h = kv.first + ": " + kv.second;
            header_list = curl_slist_append(header_list, h.c_str());
            if (kv.first == "Content-Type") has_content_type = true;
        }
        if (is_post && !has_content_type) {
            header_list = curl_slist_append(
                header_list, "Content-Type: application/json");
        }
        if (header_list) {
            curl_easy_setopt(curl, CURLOPT_HTTPHEADER, header_list);
        }

        // POST body.
        if (is_post && body) {
            curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body->c_str());
            curl_easy_setopt(
                curl, CURLOPT_POSTFIELDSIZE,
                static_cast<long>(body->size()));
        }

        // Response capture.
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &resp_body);
        curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, header_callback);
        curl_easy_setopt(curl, CURLOPT_HEADERDATA, &out.headers);

        CURLcode res = curl_easy_perform(curl);
        if (res != CURLE_OK) {
            const char* err = curl_easy_strerror(res);
            curl_slist_free_all(header_list);
            curl_easy_cleanup(curl);
            throw std::runtime_error(
                std::string("curl request failed: ") + err);
        }

        long http_code = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
        out.status_code = static_cast<int>(http_code);
        out.body = std::move(resp_body);

        curl_slist_free_all(header_list);
        curl_easy_cleanup(curl);
        return out;
    }
};

}  // namespace

std::unique_ptr<HttpTransport> make_curl_transport() {
    return std::make_unique<CurlTransport>();
}

#elif defined(HERMES_LLM_HAS_CPR)

namespace {

class CurlTransport : public HttpTransport {
public:
    Response post_json(
        const std::string& url,
        const std::unordered_map<std::string, std::string>& headers,
        const std::string& body) override {
        cpr::Header cpr_headers;
        for (const auto& kv : headers) {
            cpr_headers[kv.first] = kv.second;
        }
        if (cpr_headers.find("Content-Type") == cpr_headers.end()) {
            cpr_headers["Content-Type"] = "application/json";
        }
        auto resp = cpr::Post(
            cpr::Url{url},
            cpr_headers,
            cpr::Body{body},
            cpr::Timeout{60'000});
        Response out;
        out.status_code = static_cast<int>(resp.status_code);
        out.body = std::move(resp.text);
        for (const auto& kv : resp.header) {
            out.headers[kv.first] = kv.second;
        }
        return out;
    }
};

}  // namespace

std::unique_ptr<HttpTransport> make_curl_transport() {
    return std::make_unique<CurlTransport>();
}

#else

std::unique_ptr<HttpTransport> make_curl_transport() {
    throw std::runtime_error(
        "HTTP transport unavailable — rebuild with curl or cpr");
}

#endif

// ── Global default transport (lazy singleton) ──────────────────────────

HttpTransport* get_default_transport() {
    static std::unique_ptr<HttpTransport> instance;
    static bool tried = false;
    if (!tried) {
        tried = true;
#if defined(HERMES_LLM_HAS_CURL) || defined(HERMES_LLM_HAS_CPR)
        instance = make_curl_transport();
#endif
    }
    return instance.get();  // nullptr when no transport available
}

}  // namespace hermes::llm
