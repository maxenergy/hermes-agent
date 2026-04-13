// Phase 12 — SSRF-guarded HTTP wrapper for gateway-initiated requests.
#include <hermes/gateway/safe_fetch.hpp>

#include <hermes/core/url_safety.hpp>

namespace hermes::gateway {

namespace {

hermes::llm::HttpTransport* resolve(hermes::llm::HttpTransport* t) {
    return t ? t : hermes::llm::get_default_transport();
}

SafeFetchResult ssrf_block(const std::string& url) {
    SafeFetchResult r;
    r.error = "ssrf_blocked: refusing to contact private host: " + url;
    return r;
}

}  // namespace

SafeFetchResult safe_get(
    const std::string& url,
    const std::unordered_map<std::string, std::string>& headers,
    hermes::llm::HttpTransport* transport) {
    if (!hermes::core::url_safety::is_safe_url(url)) {
        return ssrf_block(url);
    }
    auto* t = resolve(transport);
    SafeFetchResult r;
    if (!t) {
        r.error = "no http transport available";
        return r;
    }
    try {
        auto resp = t->get(url, headers);
        r.ok = (resp.status_code >= 200 && resp.status_code < 300);
        r.status_code = resp.status_code;
        r.body = std::move(resp.body);
        return r;
    } catch (const std::exception& e) {
        r.error = std::string("transport exception: ") + e.what();
        return r;
    } catch (...) {
        r.error = "unknown transport exception";
        return r;
    }
}

SafeFetchResult safe_post_json(
    const std::string& url,
    const std::unordered_map<std::string, std::string>& headers,
    const std::string& body, hermes::llm::HttpTransport* transport) {
    if (!hermes::core::url_safety::is_safe_url(url)) {
        return ssrf_block(url);
    }
    auto* t = resolve(transport);
    SafeFetchResult r;
    if (!t) {
        r.error = "no http transport available";
        return r;
    }
    try {
        auto resp = t->post_json(url, headers, body);
        r.ok = (resp.status_code >= 200 && resp.status_code < 300);
        r.status_code = resp.status_code;
        r.body = std::move(resp.body);
        return r;
    } catch (const std::exception& e) {
        r.error = std::string("transport exception: ") + e.what();
        return r;
    } catch (...) {
        r.error = "unknown transport exception";
        return r;
    }
}

}  // namespace hermes::gateway
