// Phase 12 — Signal platform adapter implementation.
#include "signal.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>

#include <hermes/gateway/status.hpp>

namespace hermes::gateway::platforms {

namespace {
bool starts_with(const std::string& s, const std::string& prefix) {
    if (s.size() < prefix.size()) return false;
    return std::equal(prefix.begin(), prefix.end(), s.begin());
}

std::string tolower_copy(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
        return std::tolower(c);
    });
    return s;
}

bool looks_like_uuid(const std::string& s) {
    // 8-4-4-4-12 hex.
    if (s.size() != 36) return false;
    for (size_t i = 0; i < s.size(); ++i) {
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            if (s[i] != '-') return false;
        } else if (!std::isxdigit(static_cast<unsigned char>(s[i]))) {
            return false;
        }
    }
    return true;
}

bool looks_like_phone(const std::string& s) {
    if (s.size() < 2 || s[0] != '+') return false;
    for (size_t i = 1; i < s.size(); ++i) {
        if (!std::isdigit(static_cast<unsigned char>(s[i]))) return false;
    }
    return true;
}

SignalAdapter::TrustLevel parse_trust(const std::string& s) {
    std::string u = s;
    std::transform(u.begin(), u.end(), u.begin(), [](unsigned char c) {
        return std::toupper(c);
    });
    if (u == "VERIFIED" || u == "TRUSTED") {
        return SignalAdapter::TrustLevel::Verified;
    }
    if (u == "TRUSTED_UNVERIFIED" || u == "UNVERIFIED") {
        return SignalAdapter::TrustLevel::TrustedUnverified;
    }
    if (u == "UNTRUSTED") return SignalAdapter::TrustLevel::Untrusted;
    return SignalAdapter::TrustLevel::Unknown;
}
}  // namespace

SignalAdapter::SignalAdapter(Config cfg) : cfg_(std::move(cfg)) {}

SignalAdapter::SignalAdapter(Config cfg, hermes::llm::HttpTransport* transport)
    : cfg_(std::move(cfg)), transport_(transport) {}

hermes::llm::HttpTransport* SignalAdapter::get_transport() {
    if (transport_) return transport_;
    return hermes::llm::get_default_transport();
}

bool SignalAdapter::connect() {
    if (cfg_.http_url.empty() && cfg_.account.empty()) return false;

    // Token-scoped lock: account number is the credential.
    if (!cfg_.account.empty() &&
        !hermes::gateway::acquire_scoped_lock(
            hermes::gateway::platform_to_string(platform()), cfg_.account, {})) {
        return false;
    }

    // Signal REST API does not have a dedicated auth/connect endpoint;
    // we verify connectivity by checking the API is reachable.
    auto* transport = get_transport();
    if (!transport) {
        if (!cfg_.account.empty()) {
            hermes::gateway::release_scoped_lock(
                hermes::gateway::platform_to_string(platform()), cfg_.account);
        }
        return false;
    }

    try {
        auto resp = transport->get(
            cfg_.http_url + "/v1/about", {});
        return resp.status_code == 200;
    } catch (...) {
        return false;
    }
}

void SignalAdapter::disconnect() {
    if (!cfg_.account.empty()) {
        hermes::gateway::release_scoped_lock(
            hermes::gateway::platform_to_string(platform()), cfg_.account);
    }
}

bool SignalAdapter::send(const std::string& chat_id,
                         const std::string& content) {
    int expires = expiration_for(chat_id);
    return send_with_expiration(chat_id, content, expires);
}

bool SignalAdapter::send_with_expiration(const std::string& recipient,
                                         const std::string& content,
                                         int expiration_seconds) {
    auto* transport = get_transport();
    if (!transport) return false;

    std::string id = normalize_identifier(recipient);

    nlohmann::json payload = {
        {"message", content},
        {"number", cfg_.account},
    };
    // signal-cli distinguishes group vs direct recipients: groups go under
    // "group-id" (or "group_id"), direct under "recipients".  Both forms
    // are accepted by different bridge versions — set both for
    // compatibility when we detect a group.
    if (is_group_identifier(id)) {
        // Strip the "group." prefix if present — the REST API accepts
        // both forms but the canonical field is the base64 id itself.
        std::string group_id = id;
        if (starts_with(group_id, "group.")) {
            group_id = group_id.substr(6);
        }
        payload["group-id"] = group_id;
    } else {
        payload["recipients"] = nlohmann::json::array({id});
    }
    if (expiration_seconds > 0) {
        payload["expires_in_seconds"] = expiration_seconds;
    }

    try {
        auto resp = transport->post_json(
            cfg_.http_url + "/v2/send",
            {{"Content-Type", "application/json"}},
            payload.dump());
        return resp.status_code >= 200 && resp.status_code < 300;
    } catch (...) {
        return false;
    }
}

bool SignalAdapter::set_expiration(const std::string& recipient,
                                   int expiration_seconds) {
    auto* transport = get_transport();
    if (!transport) return false;
    if (cfg_.account.empty()) return false;

    // signal-cli REST: PUT /v1/configuration/{number}/settings does not
    // cover this — use the higher-level /v1/expiration/{number}/{recipient}
    // endpoint which maps to `signal-cli setExpiration`.
    nlohmann::json payload = {{"expiration", expiration_seconds}};
    std::string id = normalize_identifier(recipient);
    std::string url =
        cfg_.http_url + "/v1/expiration/" + cfg_.account + "/" + id;
    try {
        auto resp = transport->post_json(
            url, {{"Content-Type", "application/json"}}, payload.dump());
        bool ok = resp.status_code >= 200 && resp.status_code < 300;
        if (ok) remember_expiration(recipient, expiration_seconds);
        return ok;
    } catch (...) {
        return false;
    }
}

void SignalAdapter::remember_expiration(const std::string& recipient,
                                        int seconds) {
    if (seconds <= 0) {
        expiration_cache_.erase(recipient);
    } else {
        expiration_cache_[recipient] = seconds;
    }
}

int SignalAdapter::expiration_for(const std::string& recipient) const {
    auto it = expiration_cache_.find(recipient);
    if (it == expiration_cache_.end()) return 0;
    return it->second;
}

std::optional<SignalAdapter::SafetyNumber> SignalAdapter::fetch_safety_number(
    const std::string& recipient) {
    auto* transport = get_transport();
    if (!transport) return std::nullopt;
    if (cfg_.account.empty()) return std::nullopt;

    std::string url = cfg_.http_url + "/v1/identities/" + cfg_.account;
    try {
        auto resp = transport->get(url, {});
        if (resp.status_code < 200 || resp.status_code >= 300) {
            return std::nullopt;
        }
        auto j = nlohmann::json::parse(resp.body, nullptr, false);
        if (j.is_discarded() || !j.is_array()) return std::nullopt;
        std::string target = normalize_identifier(recipient);
        for (const auto& entry : j) {
            if (!entry.is_object()) continue;
            std::string num = entry.value("number", std::string{});
            std::string uuid = entry.value("uuid", std::string{});
            if (num != target && uuid != target &&
                normalize_identifier(num) != target) {
                continue;
            }
            SafetyNumber sn;
            sn.recipient = target;
            sn.fingerprint = entry.value(
                "safety_number",
                entry.value("fingerprint", std::string{}));
            sn.trust = parse_trust(
                entry.value("trust_level",
                            entry.value("trustLevel", std::string{})));
            return sn;
        }
        return std::nullopt;
    } catch (...) {
        return std::nullopt;
    }
}

bool SignalAdapter::verify_safety_number(const std::string& recipient) {
    auto sn = fetch_safety_number(recipient);
    if (!sn) return false;
    return sn->trust == TrustLevel::Verified;
}

bool SignalAdapter::trust_identity(const std::string& recipient,
                                   bool verified) {
    auto* transport = get_transport();
    if (!transport) return false;
    if (cfg_.account.empty()) return false;

    std::string id = normalize_identifier(recipient);
    std::string url = cfg_.http_url + "/v1/identities/" + cfg_.account +
                      "/trust/" + id;
    nlohmann::json payload = {{"trust_all_known_keys", !verified},
                              {"verified_safety_number", verified}};
    try {
        auto resp = transport->post_json(
            url, {{"Content-Type", "application/json"}}, payload.dump());
        return resp.status_code >= 200 && resp.status_code < 300;
    } catch (...) {
        return false;
    }
}

void SignalAdapter::send_typing(const std::string& /*chat_id*/) {}

std::string SignalAdapter::normalize_identifier(const std::string& id) {
    if (id.empty()) return id;
    // Explicit "@group" / "group:" prefix used by the runner.
    if (starts_with(id, "@group.")) return id.substr(1);
    if (starts_with(id, "group:")) return "group." + id.substr(6);
    if (starts_with(id, "group.")) return id;

    // UUIDs: lower-case them.
    if (looks_like_uuid(id)) return tolower_copy(id);

    // Phone numbers come as-is from E.164.  If missing leading +, add it
    // only when the remainder is all digits and length >= 7.
    if (looks_like_phone(id)) return id;
    if (!id.empty() && std::all_of(id.begin(), id.end(), [](unsigned char c) {
            return std::isdigit(c);
        }) && id.size() >= 7) {
        return "+" + id;
    }
    return id;
}

bool SignalAdapter::is_group_identifier(const std::string& id) {
    if (starts_with(id, "group.")) return true;
    if (starts_with(id, "@group.")) return true;
    if (starts_with(id, "group:")) return true;
    return false;
}

}  // namespace hermes::gateway::platforms
