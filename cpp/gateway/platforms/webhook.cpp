// Phase 12 — Generic Webhook adapter implementation (depth port).
#include "webhook.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <iomanip>
#include <regex>
#include <sstream>

#include <openssl/hmac.h>

namespace hermes::gateway::platforms {

namespace {

std::string to_lower(std::string s) {
    for (auto& c : s) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return s;
}

bool constant_time_eq(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    unsigned char diff = 0;
    for (std::size_t i = 0; i < a.size(); ++i) {
        diff |= static_cast<unsigned char>(a[i]) ^ static_cast<unsigned char>(b[i]);
    }
    return diff == 0;
}

std::string header_get(
    const std::unordered_map<std::string, std::string>& headers,
    const std::string& key) {
    std::string lk = to_lower(key);
    for (const auto& kv : headers) {
        if (to_lower(kv.first) == lk) return kv.second;
    }
    return {};
}

std::string truncate(const std::string& s, std::size_t n) {
    return s.size() <= n ? s : s.substr(0, n);
}

}  // namespace

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

std::string webhook_compute_hmac_sha256(const std::string& secret,
                                        const std::string& body) {
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int dlen = 0;
    HMAC(EVP_sha256(), secret.data(), static_cast<int>(secret.size()),
         reinterpret_cast<const unsigned char*>(body.data()), body.size(),
         digest, &dlen);
    std::ostringstream oss;
    oss << std::hex << std::setfill('0');
    for (unsigned int i = 0; i < dlen; ++i) {
        oss << std::setw(2) << static_cast<int>(digest[i]);
    }
    return oss.str();
}

bool webhook_validate_signature(
    const std::unordered_map<std::string, std::string>& headers,
    const std::string& body, const std::string& secret) {
    if (secret.empty()) return false;

    auto gh = header_get(headers, "X-Hub-Signature-256");
    if (!gh.empty()) {
        auto expected = "sha256=" + webhook_compute_hmac_sha256(secret, body);
        return constant_time_eq(gh, expected);
    }
    auto gl = header_get(headers, "X-Gitlab-Token");
    if (!gl.empty()) return constant_time_eq(gl, secret);
    auto generic = header_get(headers, "X-Webhook-Signature");
    if (!generic.empty()) {
        auto expected = webhook_compute_hmac_sha256(secret, body);
        return constant_time_eq(to_lower(generic), expected);
    }
    return false;
}

std::string webhook_extract_event_type(
    const std::unordered_map<std::string, std::string>& headers,
    const nlohmann::json& payload) {
    auto h = header_get(headers, "X-GitHub-Event");
    if (!h.empty()) return h;
    h = header_get(headers, "X-GitLab-Event");
    if (!h.empty()) return h;
    if (payload.is_object()) {
        auto it = payload.find("event_type");
        if (it != payload.end() && it->is_string()) {
            std::string s = it->get<std::string>();
            if (!s.empty()) return s;
        }
    }
    return "unknown";
}

std::string webhook_render_prompt(const std::string& tmpl,
                                  const nlohmann::json& payload,
                                  const std::string& event_type,
                                  const std::string& route_name) {
    if (tmpl.empty()) {
        std::string dump = truncate(payload.dump(2), 4000);
        return "Webhook event '" + event_type + "' on route '" +
               route_name + "':\n\n```json\n" + dump + "\n```";
    }
    static const std::regex token_re(R"(\{([a-zA-Z0-9_.]+)\})");
    std::string out;
    out.reserve(tmpl.size());
    auto begin = std::sregex_iterator(tmpl.begin(), tmpl.end(), token_re);
    auto end = std::sregex_iterator();
    std::size_t last = 0;
    for (auto it = begin; it != end; ++it) {
        const auto& m = *it;
        out.append(tmpl, last, static_cast<std::size_t>(m.position()) - last);
        std::string key = m[1].str();
        if (key == "__raw__") {
            out += truncate(payload.dump(2), 4000);
        } else {
            const nlohmann::json* cur = &payload;
            bool ok = true;
            std::stringstream ss(key);
            std::string part;
            while (std::getline(ss, part, '.')) {
                if (cur->is_object() && cur->contains(part)) {
                    cur = &(*cur)[part];
                } else {
                    ok = false;
                    break;
                }
            }
            if (!ok) {
                out += "{" + key + "}";
            } else if (cur->is_string()) {
                out += cur->get<std::string>();
            } else if (cur->is_object() || cur->is_array()) {
                out += truncate(cur->dump(2), 2000);
            } else {
                out += cur->dump();
            }
        }
        last = static_cast<std::size_t>(m.position() + m.length());
    }
    out.append(tmpl, last, tmpl.size() - last);
    return out;
}

nlohmann::json webhook_render_delivery_extra(const nlohmann::json& extra,
                                             const nlohmann::json& payload) {
    nlohmann::json out = nlohmann::json::object();
    if (!extra.is_object()) return out;
    for (auto it = extra.begin(); it != extra.end(); ++it) {
        if (it->is_string()) {
            out[it.key()] = webhook_render_prompt(
                it->get<std::string>(), payload, "", "");
        } else {
            out[it.key()] = *it;
        }
    }
    return out;
}

std::string webhook_build_session_chat_id(const std::string& route_name,
                                          const std::string& delivery_id) {
    return "webhook:" + route_name + ":" + delivery_id;
}

std::string webhook_extract_delivery_id(
    const std::unordered_map<std::string, std::string>& headers,
    std::chrono::system_clock::time_point now) {
    auto h = header_get(headers, "X-GitHub-Delivery");
    if (!h.empty()) return h;
    h = header_get(headers, "X-Request-ID");
    if (!h.empty()) return h;
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                  now.time_since_epoch())
                  .count();
    return std::to_string(ms);
}

std::vector<WebhookValidateError> webhook_validate_routes(
    const std::unordered_map<std::string, WebhookRoute>& routes,
    const std::string& global_secret) {
    std::vector<WebhookValidateError> errs;
    for (const auto& kv : routes) {
        std::string secret = !kv.second.secret.empty() ? kv.second.secret
                                                       : global_secret;
        if (secret.empty()) {
            errs.push_back({kv.first,
                            "no HMAC secret — set 'secret' on the route or "
                            "globally (testing: secret = 'INSECURE_NO_AUTH')"});
        }
    }
    return errs;
}

// ---------------------------------------------------------------------------
// Adapter
// ---------------------------------------------------------------------------

WebhookAdapter::WebhookAdapter(Config cfg) : cfg_(std::move(cfg)) {
    rebuild_effective_routes();
}

WebhookAdapter::WebhookAdapter(Config cfg, hermes::llm::HttpTransport* transport)
    : WebhookAdapter(std::move(cfg)) {
    transport_ = transport;
}

hermes::llm::HttpTransport* WebhookAdapter::get_transport() {
    if (transport_) return transport_;
    return hermes::llm::get_default_transport();
}

void WebhookAdapter::rebuild_effective_routes() {
    effective_routes_ = dynamic_routes_;
    for (const auto& kv : cfg_.static_routes) {
        effective_routes_[kv.first] = kv.second;
    }
}

bool WebhookAdapter::connect() {
    auto errs = webhook_validate_routes(cfg_.static_routes, cfg_.global_secret);
    if (!errs.empty()) return false;
    connected_ = true;
    return true;
}

void WebhookAdapter::disconnect() {
    connected_ = false;
}

bool WebhookAdapter::send(const std::string& chat_id,
                          const std::string& content) {
    // Outbound delivery (legacy mode).
    if (!cfg_.endpoint_url.empty()) {
        auto* t = get_transport();
        if (!t) return false;
        nlohmann::json payload = {{"chat_id", chat_id}, {"content", content}};
        std::string body = payload.dump();
        std::unordered_map<std::string, std::string> headers = {
            {"Content-Type", "application/json"}};
        if (!cfg_.signature_secret.empty()) {
            headers["X-Hermes-Signature"] =
                compute_hmac(cfg_.signature_secret, body);
        }
        try {
            auto resp = t->post_json(cfg_.endpoint_url, headers, body);
            return resp.status_code >= 200 && resp.status_code < 300;
        } catch (...) {
            return false;
        }
    }
    // Inbound mode: delivery info drives the actual response routing — out
    // of scope for this depth port, just acknowledge.
    auto info = get_delivery_info(chat_id);
    if (!info) return true;
    return true;
}

void WebhookAdapter::send_typing(const std::string& /*chat_id*/) {}

bool WebhookAdapter::verify_hmac_signature(const std::string& secret,
                                           const std::string& body,
                                           const std::string& signature) {
    auto expected = webhook_compute_hmac_sha256(secret, body);
    return constant_time_eq(to_lower(signature), expected);
}

std::string WebhookAdapter::compute_hmac(const std::string& secret,
                                         const std::string& body) {
    return webhook_compute_hmac_sha256(secret, body);
}

bool WebhookAdapter::has_route(const std::string& name) const {
    return effective_routes_.count(name) > 0;
}

void WebhookAdapter::set_dynamic_routes(
    std::unordered_map<std::string, WebhookRoute> dyn) {
    dynamic_routes_ = std::move(dyn);
    rebuild_effective_routes();
}

bool WebhookAdapter::seen_delivery(const std::string& delivery_id) const {
    std::lock_guard<std::mutex> lock(state_mu_);
    return seen_deliveries_.count(delivery_id) > 0;
}

void WebhookAdapter::mark_delivery_seen(
    const std::string& delivery_id,
    std::chrono::system_clock::time_point now) {
    std::lock_guard<std::mutex> lock(state_mu_);
    seen_deliveries_[delivery_id] = now;
}

void WebhookAdapter::prune_seen_deliveries(
    std::chrono::system_clock::time_point now) {
    std::lock_guard<std::mutex> lock(state_mu_);
    for (auto it = seen_deliveries_.begin(); it != seen_deliveries_.end();) {
        if (now - it->second > kWebhookIdempotencyTtl) {
            it = seen_deliveries_.erase(it);
        } else {
            ++it;
        }
    }
}

std::size_t WebhookAdapter::seen_size() const {
    std::lock_guard<std::mutex> lock(state_mu_);
    return seen_deliveries_.size();
}

bool WebhookAdapter::record_rate_limit(
    const std::string& route_name,
    std::chrono::system_clock::time_point now) {
    std::lock_guard<std::mutex> lock(state_mu_);
    auto& window = rate_windows_[route_name];
    while (!window.empty() &&
           now - window.front() > std::chrono::seconds(60)) {
        window.pop_front();
    }
    if (static_cast<int>(window.size()) >= cfg_.rate_limit_per_minute) {
        return false;
    }
    window.push_back(now);
    return true;
}

std::size_t WebhookAdapter::rate_window_size(
    const std::string& route_name) const {
    std::lock_guard<std::mutex> lock(state_mu_);
    auto it = rate_windows_.find(route_name);
    return it == rate_windows_.end() ? 0 : it->second.size();
}

void WebhookAdapter::store_delivery_info(
    const std::string& session_chat_id, nlohmann::json info,
    std::chrono::system_clock::time_point now) {
    std::lock_guard<std::mutex> lock(state_mu_);
    delivery_info_[session_chat_id] = {std::move(info), now};
}

std::optional<nlohmann::json> WebhookAdapter::get_delivery_info(
    const std::string& session_chat_id) const {
    std::lock_guard<std::mutex> lock(state_mu_);
    auto it = delivery_info_.find(session_chat_id);
    if (it == delivery_info_.end()) return std::nullopt;
    return std::optional<nlohmann::json>(it->second.first);
}

void WebhookAdapter::prune_delivery_info(
    std::chrono::system_clock::time_point now) {
    std::lock_guard<std::mutex> lock(state_mu_);
    for (auto it = delivery_info_.begin(); it != delivery_info_.end();) {
        if (now - it->second.second > kWebhookIdempotencyTtl) {
            it = delivery_info_.erase(it);
        } else {
            ++it;
        }
    }
}

std::size_t WebhookAdapter::delivery_info_size() const {
    std::lock_guard<std::mutex> lock(state_mu_);
    return delivery_info_.size();
}

int WebhookAdapter::check_body_size(std::size_t content_length) const {
    if (content_length > cfg_.max_body_bytes) return 413;
    return 0;
}

}  // namespace hermes::gateway::platforms
