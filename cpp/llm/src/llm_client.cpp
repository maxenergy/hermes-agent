// Base HTTP transport implementations: real (curl/cpr) + fake.
#include "hermes/llm/llm_client.hpp"

#include <memory>
#include <stdexcept>
#include <utility>

#if defined(HERMES_LLM_HAS_CPR)
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

// ── Real HTTP transport ────────────────────────────────────────────────

namespace {

class CurlTransport : public HttpTransport {
public:
    Response post_json(
        const std::string& url,
        const std::unordered_map<std::string, std::string>& headers,
        const std::string& body) override {
#if defined(HERMES_LLM_HAS_CPR)
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
#else
        (void)url;
        (void)headers;
        (void)body;
        throw std::runtime_error(
            "HTTP transport unavailable — rebuild with cpr");
#endif
    }
};

}  // namespace

std::unique_ptr<HttpTransport> make_curl_transport() {
    return std::make_unique<CurlTransport>();
}

}  // namespace hermes::llm
