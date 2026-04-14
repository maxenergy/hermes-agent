// Phase 12 — Matrix platform adapter implementation.
//
// Full C++17 port of gateway/platforms/matrix.py.  See matrix.hpp for the
// surface summary.  The E2EE primitives live in olm_session.{hpp,cpp}; this
// file is purely REST / JSON glue on top of them.
#include "matrix.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <regex>
#include <sstream>
#include <system_error>

#include <hermes/core/path.hpp>
#include <hermes/gateway/status.hpp>

namespace hermes::gateway::platforms {

using json = nlohmann::json;

namespace {

std::string matrix_lock_identity(const MatrixAdapter::Config& cfg) {
    return cfg.homeserver + "/" + cfg.username;
}

std::string url_encode(const std::string& s) {
    std::string out;
    out.reserve(s.size() * 3);
    for (unsigned char c : s) {
        bool unreserved = std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~';
        if (unreserved) {
            out.push_back(static_cast<char>(c));
        } else {
            char buf[4];
            std::snprintf(buf, sizeof(buf), "%%%02X", c);
            out.append(buf);
        }
    }
    return out;
}

std::string rstrip_slash(std::string s) {
    while (!s.empty() && s.back() == '/') s.pop_back();
    return s;
}

bool starts_with(const std::string& s, const std::string& p) {
    return s.size() >= p.size() && std::equal(p.begin(), p.end(), s.begin());
}

std::string to_lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string strip_ws(const std::string& s) {
    std::size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    return s.substr(a, b - a);
}

std::atomic<std::uint64_t> g_global_txn_counter{0};

}  // namespace

// ── Construction ─────────────────────────────────────────────────────────

MatrixAdapter::MatrixAdapter(Config cfg) : cfg_(std::move(cfg)) {
    cfg_.homeserver = rstrip_slash(cfg_.homeserver);
    access_token_ = cfg_.access_token;
}

MatrixAdapter::MatrixAdapter(Config cfg, hermes::llm::HttpTransport* transport)
    : cfg_(std::move(cfg)), transport_(transport) {
    cfg_.homeserver = rstrip_slash(cfg_.homeserver);
    access_token_ = cfg_.access_token;
}

hermes::llm::HttpTransport* MatrixAdapter::get_transport() {
    if (transport_) return transport_;
    return hermes::llm::get_default_transport();
}

std::string MatrixAdapter::olm_pickle_path() const {
    std::error_code ec;
    auto dir = hermes::core::path::get_hermes_home() / "matrix";
    std::filesystem::create_directories(dir, ec);
    return (dir / "olm_account.pickle").string();
}

std::string MatrixAdapter::sync_token_path() const {
    std::error_code ec;
    auto dir = hermes::core::path::get_hermes_home() / "matrix";
    std::filesystem::create_directories(dir, ec);
    return (dir / "sync_token.txt").string();
}

std::string MatrixAdapter::participated_threads_path() const {
    std::error_code ec;
    auto dir = hermes::core::path::get_hermes_home() / "matrix";
    std::filesystem::create_directories(dir, ec);
    return (dir / "participated_threads.txt").string();
}

std::string MatrixAdapter::v3_url(const std::string& suffix) const {
    return cfg_.homeserver + "/_matrix/client/v3/" + suffix;
}

std::string MatrixAdapter::v1_media_url(const std::string& suffix) const {
    return cfg_.homeserver + "/_matrix/client/v1/media/" + suffix;
}

// ── Runner contract ──────────────────────────────────────────────────────

bool MatrixAdapter::connect() {
    if (cfg_.homeserver.empty()) return false;
    if (cfg_.access_token.empty() && (cfg_.username.empty() || cfg_.password.empty())) {
        return false;
    }

    if (!hermes::gateway::acquire_scoped_lock(
            hermes::gateway::platform_to_string(platform()),
            matrix_lock_identity(cfg_), {})) {
        return false;
    }

    auto* transport = get_transport();
    if (!transport) {
        hermes::gateway::release_scoped_lock(
            hermes::gateway::platform_to_string(platform()),
            matrix_lock_identity(cfg_));
        return false;
    }

    if (!cfg_.access_token.empty()) {
        access_token_ = cfg_.access_token;
        (void)setup_e2ee();
        (void)load_sync_token();
        (void)load_participated_threads();
        return true;
    }

    auto token = login_password();
    if (token.empty()) {
        hermes::gateway::release_scoped_lock(
            hermes::gateway::platform_to_string(platform()),
            matrix_lock_identity(cfg_));
        return false;
    }
    (void)setup_e2ee();
    (void)load_sync_token();
    (void)load_participated_threads();
    return true;
}

void MatrixAdapter::disconnect() {
    (void)save_sync_token();
    (void)save_participated_threads();
    access_token_.clear();
    if (!cfg_.homeserver.empty()) {
        hermes::gateway::release_scoped_lock(
            hermes::gateway::platform_to_string(platform()),
            matrix_lock_identity(cfg_));
    }
}

bool MatrixAdapter::send(const std::string& chat_id, const std::string& content) {
    auto result = send_markdown(chat_id, content);
    return result.success;
}

void MatrixAdapter::send_typing(const std::string& chat_id) {
    auto* transport = get_transport();
    if (!transport || access_token_.empty() || chat_id.empty()) return;

    std::string who = cfg_.user_id.empty() ? std::string("@me:matrix.org") : cfg_.user_id;
    json payload = {{"typing", true}, {"timeout", 30000}};
    std::string url = v3_url("rooms/" + url_encode(chat_id) + "/typing/" + url_encode(who));

    try {
        (void)authed_post(url, payload.dump());
    } catch (...) {
        // Best-effort.
    }
}

// ── Authentication ───────────────────────────────────────────────────────

std::string MatrixAdapter::discover_homeserver() {
    auto* transport = get_transport();
    if (!transport || cfg_.homeserver.empty()) return cfg_.homeserver;
    try {
        auto resp = transport->get(cfg_.homeserver + "/.well-known/matrix/client", {});
        if (resp.status_code >= 200 && resp.status_code < 300) {
            auto body = json::parse(resp.body, nullptr, false);
            if (!body.is_discarded() && body.contains("m.homeserver")) {
                auto hs = body["m.homeserver"];
                if (hs.is_object() && hs.contains("base_url")) {
                    auto url = hs["base_url"].get<std::string>();
                    url = rstrip_slash(url);
                    if (!url.empty()) cfg_.homeserver = url;
                }
            }
        }
    } catch (...) {
        // pass through
    }
    return cfg_.homeserver;
}

std::string MatrixAdapter::homeserver_from_mxid(const std::string& mxid) {
    // `@user:example.org` → `https://example.org`
    auto colon = mxid.find(':');
    if (colon == std::string::npos) return {};
    auto host = mxid.substr(colon + 1);
    if (host.empty()) return {};
    return "https://" + host;
}

std::string MatrixAdapter::login_password() {
    auto* transport = get_transport();
    if (!transport) return {};
    json payload = {
        {"type", "m.login.password"},
        {"identifier", {{"type", "m.id.user"}, {"user", cfg_.username}}},
        {"password", cfg_.password},
        {"initial_device_display_name", cfg_.device_name},
    };
    if (!cfg_.device_id.empty()) payload["device_id"] = cfg_.device_id;

    try {
        auto resp = transport->post_json(
            v3_url("login"),
            {{"Content-Type", "application/json"}},
            payload.dump());
        if (resp.status_code < 200 || resp.status_code >= 300) return {};
        auto body = json::parse(resp.body, nullptr, false);
        if (body.is_discarded() || !body.contains("access_token")) return {};
        access_token_ = body["access_token"].get<std::string>();
        if (body.contains("device_id")) cfg_.device_id = body["device_id"].get<std::string>();
        if (body.contains("user_id")) cfg_.user_id = body["user_id"].get<std::string>();
        if (body.contains("refresh_token")) {
            cfg_.refresh_token = body["refresh_token"].get<std::string>();
        }
        return access_token_;
    } catch (...) {
        return {};
    }
}

std::string MatrixAdapter::login_sso_token(const std::string& login_token) {
    auto* transport = get_transport();
    if (!transport || login_token.empty()) return {};
    json payload = {
        {"type", "m.login.token"},
        {"token", login_token},
        {"initial_device_display_name", cfg_.device_name},
    };
    try {
        auto resp = transport->post_json(
            v3_url("login"),
            {{"Content-Type", "application/json"}},
            payload.dump());
        if (resp.status_code < 200 || resp.status_code >= 300) return {};
        auto body = json::parse(resp.body, nullptr, false);
        if (body.is_discarded() || !body.contains("access_token")) return {};
        access_token_ = body["access_token"].get<std::string>();
        if (body.contains("user_id")) cfg_.user_id = body["user_id"].get<std::string>();
        if (body.contains("device_id")) cfg_.device_id = body["device_id"].get<std::string>();
        return access_token_;
    } catch (...) {
        return {};
    }
}

std::string MatrixAdapter::sso_redirect_url(const std::string& redirect_to) const {
    return v3_url("login/sso/redirect") + "?redirectUrl=" + url_encode(redirect_to);
}

bool MatrixAdapter::refresh_access_token() {
    auto* transport = get_transport();
    if (!transport || cfg_.refresh_token.empty()) return false;
    json payload = {{"refresh_token", cfg_.refresh_token}};
    try {
        auto resp = transport->post_json(
            v3_url("refresh"),
            {{"Content-Type", "application/json"}},
            payload.dump());
        if (resp.status_code < 200 || resp.status_code >= 300) return false;
        auto body = json::parse(resp.body, nullptr, false);
        if (body.is_discarded() || !body.contains("access_token")) return false;
        access_token_ = body["access_token"].get<std::string>();
        if (body.contains("refresh_token")) {
            cfg_.refresh_token = body["refresh_token"].get<std::string>();
        }
        return true;
    } catch (...) {
        return false;
    }
}

bool MatrixAdapter::logout() {
    auto* transport = get_transport();
    if (!transport || access_token_.empty()) {
        access_token_.clear();
        return true;
    }
    try {
        auto resp = authed_post(v3_url("logout"), "{}");
        access_token_.clear();
        return resp.status_code >= 200 && resp.status_code < 300;
    } catch (...) {
        access_token_.clear();
        return false;
    }
}

// ── Authed request helpers ───────────────────────────────────────────────

hermes::llm::HttpTransport::Response MatrixAdapter::authed_post(
    const std::string& url, const std::string& body,
    const std::string& extra_content_type) {
    auto* transport = get_transport();
    hermes::llm::HttpTransport::Response r;
    if (!transport) return r;
    std::unordered_map<std::string, std::string> headers = {
        {"Authorization", "Bearer " + access_token_},
        {"Content-Type", extra_content_type},
    };
    return transport->post_json(url, headers, body);
}

hermes::llm::HttpTransport::Response MatrixAdapter::authed_get(const std::string& url) {
    auto* transport = get_transport();
    hermes::llm::HttpTransport::Response r;
    if (!transport) return r;
    std::unordered_map<std::string, std::string> headers = {
        {"Authorization", "Bearer " + access_token_},
    };
    return transport->get(url, headers);
}

hermes::llm::HttpTransport::Response MatrixAdapter::retrying_post(
    const std::string& url, const std::string& body, int max_retries) {
    hermes::llm::HttpTransport::Response resp;
    for (int attempt = 0; attempt <= max_retries; ++attempt) {
        try {
            resp = authed_post(url, body);
        } catch (...) {
            resp.status_code = 0;
            resp.body.clear();
            return resp;
        }
        if (resp.status_code != 429) return resp;
        // Parse retry_after_ms.  We don't sleep here (test-friendly); we
        // just record the state and return the 429.  Callers can decide.
        auto parsed = json::parse(resp.body, nullptr, false);
        std::int64_t retry_ms = 1000;
        if (!parsed.is_discarded() && parsed.contains("retry_after_ms")) {
            retry_ms = parsed["retry_after_ms"].get<std::int64_t>();
        }
        record_rate_limit(retry_ms);
        if (attempt == max_retries) return resp;
    }
    return resp;
}

void MatrixAdapter::record_rate_limit(std::int64_t retry_after_ms) {
    rate_limit_.retry_after_ms = retry_after_ms;
    rate_limit_.hit_count += 1;
}

// ── Payload builders ─────────────────────────────────────────────────────

std::string MatrixAdapter::build_message_payload(const std::string& msgtype,
                                                 const std::string& body,
                                                 const std::string& html,
                                                 const std::string& reply_to,
                                                 const std::string& thread_id) {
    json j = {{"msgtype", msgtype}, {"body", body}};
    if (!html.empty() && html != body) {
        j["format"] = "org.matrix.custom.html";
        j["formatted_body"] = html;
    }
    if (!reply_to.empty() || !thread_id.empty()) {
        json relates_to = json::object();
        if (!reply_to.empty()) {
            relates_to["m.in_reply_to"] = {{"event_id", reply_to}};
        }
        if (!thread_id.empty()) {
            relates_to["rel_type"] = "m.thread";
            relates_to["event_id"] = thread_id;
            relates_to["is_falling_back"] = true;
            if (!reply_to.empty() && !relates_to.contains("m.in_reply_to")) {
                relates_to["m.in_reply_to"] = {{"event_id", reply_to}};
            }
        }
        j["m.relates_to"] = relates_to;
    }
    return j.dump();
}

std::string MatrixAdapter::build_edit_payload(const std::string& event_id,
                                              const std::string& new_body,
                                              const std::string& new_html) {
    json new_content = {{"msgtype", "m.text"}, {"body", new_body}};
    if (!new_html.empty() && new_html != new_body) {
        new_content["format"] = "org.matrix.custom.html";
        new_content["formatted_body"] = new_html;
    }
    json j = {
        {"msgtype", "m.text"},
        {"body", std::string("* ") + new_body},
        {"m.new_content", new_content},
        {"m.relates_to", {{"rel_type", "m.replace"}, {"event_id", event_id}}},
    };
    if (!new_html.empty() && new_html != new_body) {
        j["format"] = "org.matrix.custom.html";
        j["formatted_body"] = std::string("* ") + new_html;
    }
    return j.dump();
}

std::string MatrixAdapter::build_reaction_payload(const std::string& target_event_id,
                                                  const std::string& key) {
    json j = {
        {"m.relates_to",
         {{"rel_type", "m.annotation"}, {"event_id", target_event_id}, {"key", key}}},
    };
    return j.dump();
}

std::vector<std::string> MatrixAdapter::chunk_message(const std::string& body,
                                                     std::size_t max_len) {
    std::vector<std::string> out;
    if (max_len == 0 || body.size() <= max_len) {
        out.push_back(body);
        return out;
    }
    std::size_t i = 0;
    while (i < body.size()) {
        std::size_t end = std::min(i + max_len, body.size());
        if (end < body.size()) {
            // Scan back for whitespace so we don't split words.
            std::size_t scan = end;
            while (scan > i && !std::isspace(static_cast<unsigned char>(body[scan - 1]))) {
                --scan;
            }
            if (scan > i) end = scan;
        }
        out.push_back(body.substr(i, end - i));
        i = end;
    }
    return out;
}

// ── Messaging ────────────────────────────────────────────────────────────

MatrixSendResult MatrixAdapter::send_text(const std::string& room_id,
                                          const std::string& body,
                                          const std::string& reply_to,
                                          const std::string& thread_id) {
    if (body.empty()) return MatrixSendResult::ok();
    if (access_token_.empty()) return MatrixSendResult::fail("not authenticated");

    auto chunks = chunk_message(body, cfg_.max_message_length);
    std::string last_id;
    for (const auto& chunk : chunks) {
        auto payload = build_message_payload("m.text", chunk, /*html=*/{},
                                             reply_to, thread_id);
        std::string txn = next_txn_id();
        std::string url = v3_url("rooms/" + url_encode(room_id) +
                                 "/send/m.room.message/" + url_encode(txn));
        auto resp = retrying_post(url, payload);
        if (resp.status_code < 200 || resp.status_code >= 300) {
            return MatrixSendResult::fail("http " + std::to_string(resp.status_code));
        }
        auto parsed = json::parse(resp.body, nullptr, false);
        if (!parsed.is_discarded() && parsed.contains("event_id")) {
            last_id = parsed["event_id"].get<std::string>();
        }
    }
    if (!thread_id.empty()) track_thread(thread_id);
    return MatrixSendResult::ok(last_id);
}

MatrixSendResult MatrixAdapter::send_html(const std::string& room_id,
                                          const std::string& body,
                                          const std::string& html,
                                          const std::string& reply_to,
                                          const std::string& thread_id) {
    if (body.empty() && html.empty()) return MatrixSendResult::ok();
    if (access_token_.empty()) return MatrixSendResult::fail("not authenticated");
    auto payload = build_message_payload("m.text", body, html, reply_to, thread_id);
    std::string txn = next_txn_id();
    std::string url = v3_url("rooms/" + url_encode(room_id) +
                             "/send/m.room.message/" + url_encode(txn));
    auto resp = retrying_post(url, payload);
    if (resp.status_code < 200 || resp.status_code >= 300) {
        return MatrixSendResult::fail("http " + std::to_string(resp.status_code));
    }
    auto parsed = json::parse(resp.body, nullptr, false);
    std::string id;
    if (!parsed.is_discarded() && parsed.contains("event_id")) {
        id = parsed["event_id"].get<std::string>();
    }
    if (!thread_id.empty()) track_thread(thread_id);
    return MatrixSendResult::ok(id);
}

MatrixSendResult MatrixAdapter::send_markdown(const std::string& room_id,
                                              const std::string& markdown_text,
                                              const std::string& reply_to,
                                              const std::string& thread_id) {
    auto html = markdown_to_html(markdown_text);
    if (html == markdown_text) {
        return send_text(room_id, markdown_text, reply_to, thread_id);
    }
    return send_html(room_id, markdown_text, html, reply_to, thread_id);
}

MatrixSendResult MatrixAdapter::send_notice(const std::string& room_id,
                                            const std::string& body) {
    if (access_token_.empty()) return MatrixSendResult::fail("not authenticated");
    auto html = markdown_to_html(body);
    auto payload = build_message_payload("m.notice", body,
                                         html == body ? std::string() : html,
                                         /*reply_to=*/{}, /*thread_id=*/{});
    std::string txn = next_txn_id();
    std::string url = v3_url("rooms/" + url_encode(room_id) +
                             "/send/m.room.message/" + url_encode(txn));
    auto resp = retrying_post(url, payload);
    if (resp.status_code < 200 || resp.status_code >= 300) {
        return MatrixSendResult::fail("http " + std::to_string(resp.status_code));
    }
    auto parsed = json::parse(resp.body, nullptr, false);
    std::string id;
    if (!parsed.is_discarded() && parsed.contains("event_id")) {
        id = parsed["event_id"].get<std::string>();
    }
    return MatrixSendResult::ok(id);
}

MatrixSendResult MatrixAdapter::send_emote(const std::string& room_id,
                                           const std::string& body) {
    if (access_token_.empty()) return MatrixSendResult::fail("not authenticated");
    auto html = markdown_to_html(body);
    auto payload = build_message_payload("m.emote", body,
                                         html == body ? std::string() : html,
                                         /*reply_to=*/{}, /*thread_id=*/{});
    std::string txn = next_txn_id();
    std::string url = v3_url("rooms/" + url_encode(room_id) +
                             "/send/m.room.message/" + url_encode(txn));
    auto resp = retrying_post(url, payload);
    if (resp.status_code < 200 || resp.status_code >= 300) {
        return MatrixSendResult::fail("http " + std::to_string(resp.status_code));
    }
    auto parsed = json::parse(resp.body, nullptr, false);
    std::string id;
    if (!parsed.is_discarded() && parsed.contains("event_id")) {
        id = parsed["event_id"].get<std::string>();
    }
    return MatrixSendResult::ok(id);
}

MatrixSendResult MatrixAdapter::edit_message(const std::string& room_id,
                                             const std::string& event_id,
                                             const std::string& new_body) {
    if (access_token_.empty()) return MatrixSendResult::fail("not authenticated");
    auto html = markdown_to_html(new_body);
    auto payload = build_edit_payload(event_id, new_body,
                                      html == new_body ? std::string() : html);
    std::string txn = next_txn_id();
    std::string url = v3_url("rooms/" + url_encode(room_id) +
                             "/send/m.room.message/" + url_encode(txn));
    auto resp = retrying_post(url, payload);
    if (resp.status_code < 200 || resp.status_code >= 300) {
        return MatrixSendResult::fail("http " + std::to_string(resp.status_code));
    }
    auto parsed = json::parse(resp.body, nullptr, false);
    std::string id;
    if (!parsed.is_discarded() && parsed.contains("event_id")) {
        id = parsed["event_id"].get<std::string>();
    }
    return MatrixSendResult::ok(id);
}

bool MatrixAdapter::redact_message(const std::string& room_id,
                                   const std::string& event_id,
                                   const std::string& reason) {
    if (access_token_.empty()) return false;
    json payload = json::object();
    if (!reason.empty()) payload["reason"] = reason;
    std::string txn = next_txn_id();
    std::string url = v3_url("rooms/" + url_encode(room_id) + "/redact/" +
                             url_encode(event_id) + "/" + url_encode(txn));
    try {
        auto resp = authed_post(url, payload.dump());
        return resp.status_code >= 200 && resp.status_code < 300;
    } catch (...) {
        return false;
    }
}

MatrixSendResult MatrixAdapter::send_reaction(const std::string& room_id,
                                              const std::string& target_event_id,
                                              const std::string& key) {
    if (access_token_.empty()) return MatrixSendResult::fail("not authenticated");
    auto payload = build_reaction_payload(target_event_id, key);
    std::string txn = next_txn_id();
    std::string url = v3_url("rooms/" + url_encode(room_id) +
                             "/send/m.reaction/" + url_encode(txn));
    auto resp = retrying_post(url, payload);
    if (resp.status_code < 200 || resp.status_code >= 300) {
        return MatrixSendResult::fail("http " + std::to_string(resp.status_code));
    }
    auto parsed = json::parse(resp.body, nullptr, false);
    std::string id;
    if (!parsed.is_discarded() && parsed.contains("event_id")) {
        id = parsed["event_id"].get<std::string>();
    }
    return MatrixSendResult::ok(id);
}

// ── Media ────────────────────────────────────────────────────────────────

std::string MatrixAdapter::upload_media(const std::string& content_type,
                                        const std::string& filename,
                                        const std::string& bytes) {
    auto* transport = get_transport();
    if (!transport || access_token_.empty()) return {};
    std::string url = cfg_.homeserver + "/_matrix/media/v3/upload?filename=" +
                      url_encode(filename);
    std::unordered_map<std::string, std::string> headers = {
        {"Authorization", "Bearer " + access_token_},
        {"Content-Type", content_type.empty() ? "application/octet-stream" : content_type},
    };
    try {
        auto resp = transport->post_json(url, headers, bytes);
        if (resp.status_code < 200 || resp.status_code >= 300) return {};
        auto parsed = json::parse(resp.body, nullptr, false);
        if (parsed.is_discarded() || !parsed.contains("content_uri")) return {};
        return parsed["content_uri"].get<std::string>();
    } catch (...) {
        return {};
    }
}

MatrixSendResult MatrixAdapter::send_media(const std::string& room_id,
                                           const std::string& msgtype,
                                           const std::string& mxc_url,
                                           const std::string& filename,
                                           const std::string& content_type,
                                           std::size_t size_bytes,
                                           const std::string& caption,
                                           const std::string& reply_to) {
    if (access_token_.empty()) return MatrixSendResult::fail("not authenticated");
    json info = json::object();
    if (!content_type.empty()) info["mimetype"] = content_type;
    if (size_bytes > 0) info["size"] = size_bytes;
    json payload = {
        {"msgtype", msgtype},
        {"body", caption.empty() ? filename : caption},
        {"filename", filename},
        {"url", mxc_url},
        {"info", info},
    };
    if (!reply_to.empty()) {
        payload["m.relates_to"] = {{"m.in_reply_to", {{"event_id", reply_to}}}};
    }
    std::string txn = next_txn_id();
    std::string url = v3_url("rooms/" + url_encode(room_id) +
                             "/send/m.room.message/" + url_encode(txn));
    auto resp = retrying_post(url, payload.dump());
    if (resp.status_code < 200 || resp.status_code >= 300) {
        return MatrixSendResult::fail("http " + std::to_string(resp.status_code));
    }
    auto parsed = json::parse(resp.body, nullptr, false);
    std::string id;
    if (!parsed.is_discarded() && parsed.contains("event_id")) {
        id = parsed["event_id"].get<std::string>();
    }
    return MatrixSendResult::ok(id);
}

std::string MatrixAdapter::mxc_to_http(const std::string& mxc_url) const {
    if (!starts_with(mxc_url, "mxc://")) return mxc_url;
    return cfg_.homeserver + "/_matrix/client/v1/media/download/" + mxc_url.substr(6);
}

// ── Room ops ─────────────────────────────────────────────────────────────

std::string MatrixAdapter::create_room(const std::string& name,
                                       const std::string& topic,
                                       const std::vector<std::string>& invitees,
                                       bool is_direct,
                                       const std::string& preset) {
    if (access_token_.empty()) return {};
    json payload = json::object();
    if (!name.empty()) payload["name"] = name;
    if (!topic.empty()) payload["topic"] = topic;
    payload["is_direct"] = is_direct;
    payload["preset"] = preset;
    if (!invitees.empty()) payload["invite"] = invitees;

    try {
        auto resp = authed_post(v3_url("createRoom"), payload.dump());
        if (resp.status_code < 200 || resp.status_code >= 300) return {};
        auto parsed = json::parse(resp.body, nullptr, false);
        if (parsed.is_discarded() || !parsed.contains("room_id")) return {};
        auto rid = parsed["room_id"].get<std::string>();
        if (is_direct && !invitees.empty()) {
            dm_rooms_[invitees.front()] = rid;
        }
        return rid;
    } catch (...) {
        return {};
    }
}

std::string MatrixAdapter::ensure_dm_room(const std::string& user_id) {
    auto it = dm_rooms_.find(user_id);
    if (it != dm_rooms_.end()) return it->second;
    return create_room(/*name=*/{}, /*topic=*/{}, {user_id}, /*is_direct=*/true,
                       /*preset=*/"trusted_private_chat");
}

bool MatrixAdapter::join_room(const std::string& room_id_or_alias) {
    if (access_token_.empty()) return false;
    auto resp = authed_post(v3_url("join/" + url_encode(room_id_or_alias)), "{}");
    return resp.status_code >= 200 && resp.status_code < 300;
}

bool MatrixAdapter::leave_room(const std::string& room_id) {
    if (access_token_.empty()) return false;
    auto resp = authed_post(v3_url("rooms/" + url_encode(room_id) + "/leave"), "{}");
    return resp.status_code >= 200 && resp.status_code < 300;
}

bool MatrixAdapter::invite_user(const std::string& room_id, const std::string& user_id) {
    if (access_token_.empty()) return false;
    json payload = {{"user_id", user_id}};
    auto resp = authed_post(v3_url("rooms/" + url_encode(room_id) + "/invite"),
                            payload.dump());
    return resp.status_code >= 200 && resp.status_code < 300;
}

bool MatrixAdapter::kick_user(const std::string& room_id, const std::string& user_id,
                              const std::string& reason) {
    if (access_token_.empty()) return false;
    json payload = {{"user_id", user_id}};
    if (!reason.empty()) payload["reason"] = reason;
    auto resp = authed_post(v3_url("rooms/" + url_encode(room_id) + "/kick"),
                            payload.dump());
    return resp.status_code >= 200 && resp.status_code < 300;
}

bool MatrixAdapter::ban_user(const std::string& room_id, const std::string& user_id,
                             const std::string& reason) {
    if (access_token_.empty()) return false;
    json payload = {{"user_id", user_id}};
    if (!reason.empty()) payload["reason"] = reason;
    auto resp = authed_post(v3_url("rooms/" + url_encode(room_id) + "/ban"),
                            payload.dump());
    return resp.status_code >= 200 && resp.status_code < 300;
}

bool MatrixAdapter::set_room_name(const std::string& room_id, const std::string& name) {
    if (access_token_.empty()) return false;
    json payload = {{"name", name}};
    auto resp = authed_post(v3_url("rooms/" + url_encode(room_id) +
                                   "/state/m.room.name/"),
                            payload.dump());
    return resp.status_code >= 200 && resp.status_code < 300;
}

bool MatrixAdapter::set_room_topic(const std::string& room_id, const std::string& topic) {
    if (access_token_.empty()) return false;
    json payload = {{"topic", topic}};
    auto resp = authed_post(v3_url("rooms/" + url_encode(room_id) +
                                   "/state/m.room.topic/"),
                            payload.dump());
    return resp.status_code >= 200 && resp.status_code < 300;
}

bool MatrixAdapter::set_power_level(const std::string& room_id,
                                    const std::string& user_id, int level) {
    if (access_token_.empty()) return false;
    // Fetch current power-levels, mutate, re-upload.
    auto resp = authed_get(v3_url("rooms/" + url_encode(room_id) +
                                  "/state/m.room.power_levels/"));
    if (resp.status_code < 200 || resp.status_code >= 300) return false;
    auto body = json::parse(resp.body, nullptr, false);
    if (body.is_discarded()) body = json::object();
    if (!body.contains("users") || !body["users"].is_object()) {
        body["users"] = json::object();
    }
    body["users"][user_id] = level;
    auto put_resp = authed_post(v3_url("rooms/" + url_encode(room_id) +
                                       "/state/m.room.power_levels/"),
                                body.dump());
    return put_resp.status_code >= 200 && put_resp.status_code < 300;
}

bool MatrixAdapter::send_read_receipt(const std::string& room_id,
                                      const std::string& event_id) {
    if (access_token_.empty()) return false;
    auto resp = authed_post(v3_url("rooms/" + url_encode(room_id) +
                                   "/receipt/m.read/" + url_encode(event_id)),
                            "{}");
    return resp.status_code >= 200 && resp.status_code < 300;
}

std::pair<std::string, std::string> MatrixAdapter::get_chat_info(
    const std::string& room_id) {
    std::string name = room_id;
    std::string type = "group";
    if (access_token_.empty()) return {name, type};
    try {
        auto resp = authed_get(v3_url("rooms/" + url_encode(room_id) +
                                      "/state/m.room.name/"));
        if (resp.status_code >= 200 && resp.status_code < 300) {
            auto body = json::parse(resp.body, nullptr, false);
            if (!body.is_discarded() && body.contains("name")) {
                name = body["name"].get<std::string>();
                room_names_[room_id] = name;
            }
        }
    } catch (...) {
        // pass through
    }
    for (const auto& [uid, rid] : dm_rooms_) {
        if (rid == room_id) {
            type = "dm";
            break;
        }
    }
    return {name, type};
}

std::vector<MatrixEvent> MatrixAdapter::fetch_room_history(const std::string& room_id,
                                                           int limit,
                                                           const std::string& start) {
    std::vector<MatrixEvent> out;
    if (access_token_.empty()) return out;
    std::string url = v3_url("rooms/" + url_encode(room_id) +
                             "/messages?dir=b&limit=" + std::to_string(limit));
    if (!start.empty()) url += "&from=" + url_encode(start);
    try {
        auto resp = authed_get(url);
        if (resp.status_code < 200 || resp.status_code >= 300) return out;
        auto body = json::parse(resp.body, nullptr, false);
        if (body.is_discarded() || !body.contains("chunk")) return out;
        for (const auto& ev : body["chunk"]) {
            MatrixEvent me;
            if (ev.contains("event_id")) me.event_id = ev["event_id"].get<std::string>();
            if (ev.contains("sender")) me.sender = ev["sender"].get<std::string>();
            if (ev.contains("type")) me.type = ev["type"].get<std::string>();
            if (ev.contains("origin_server_ts")) {
                me.timestamp = ev["origin_server_ts"].get<std::int64_t>();
            }
            me.room_id = room_id;
            if (ev.contains("content") && ev["content"].is_object()) {
                const auto& c = ev["content"];
                if (c.contains("body")) me.body = c["body"].get<std::string>();
                if (c.contains("msgtype")) me.msgtype = c["msgtype"].get<std::string>();
                if (c.contains("format")) me.format = c["format"].get<std::string>();
                if (c.contains("formatted_body")) {
                    me.formatted_body = c["formatted_body"].get<std::string>();
                }
            }
            out.push_back(std::move(me));
        }
    } catch (...) {
        return out;
    }
    std::reverse(out.begin(), out.end());
    return out;
}

// ── Presence ─────────────────────────────────────────────────────────────

bool MatrixAdapter::valid_presence_state(const std::string& state) {
    return state == "online" || state == "offline" || state == "unavailable";
}

bool MatrixAdapter::set_presence(const std::string& state,
                                 const std::string& status_msg) {
    if (access_token_.empty() || !valid_presence_state(state)) return false;
    if (cfg_.user_id.empty()) return false;
    json payload = {{"presence", state}};
    if (!status_msg.empty()) payload["status_msg"] = status_msg;
    auto resp = authed_post(v3_url("presence/" + url_encode(cfg_.user_id) +
                                   "/status"),
                            payload.dump());
    return resp.status_code >= 200 && resp.status_code < 300;
}

// ── Sync ─────────────────────────────────────────────────────────────────

bool MatrixAdapter::sync_once(MatrixSyncResponse& out, bool use_filter) {
    auto* transport = get_transport();
    if (!transport || access_token_.empty()) return false;
    std::string url = v3_url("sync?timeout=") + std::to_string(cfg_.sync_timeout_ms);
    if (!next_batch_.empty()) url += "&since=" + url_encode(next_batch_);
    if (use_filter) {
        // Narrow filter — room.timeline limited, leave other sections small.
        url += "&filter=" + url_encode(
            "{\"room\":{\"timeline\":{\"limit\":50}}}");
    }
    try {
        auto resp = authed_get(url);
        if (resp.status_code < 200 || resp.status_code >= 300) return false;
        out = parse_sync_response(resp.body);
        next_batch_ = out.next_batch;
        return true;
    } catch (...) {
        return false;
    }
}

MatrixSyncResponse MatrixAdapter::parse_sync_response(const std::string& json_body) {
    MatrixSyncResponse r;
    auto body = json::parse(json_body, nullptr, false);
    if (body.is_discarded()) return r;
    if (body.contains("next_batch")) r.next_batch = body["next_batch"].get<std::string>();

    // Joined rooms → timeline events.
    if (body.contains("rooms") && body["rooms"].is_object()) {
        const auto& rooms = body["rooms"];
        if (rooms.contains("join") && rooms["join"].is_object()) {
            for (auto it = rooms["join"].begin(); it != rooms["join"].end(); ++it) {
                const std::string room_id = it.key();
                const auto& room_body = it.value();
                std::vector<MatrixEvent> evs;
                if (room_body.contains("timeline") &&
                    room_body["timeline"].contains("events")) {
                    for (const auto& ev : room_body["timeline"]["events"]) {
                        MatrixEvent me;
                        me.room_id = room_id;
                        if (ev.contains("event_id")) me.event_id = ev["event_id"].get<std::string>();
                        if (ev.contains("sender")) me.sender = ev["sender"].get<std::string>();
                        if (ev.contains("type")) me.type = ev["type"].get<std::string>();
                        if (ev.contains("origin_server_ts")) {
                            me.timestamp = ev["origin_server_ts"].get<std::int64_t>();
                        }
                        if (ev.contains("state_key")) {
                            me.state_key = ev["state_key"].get<std::string>();
                        }
                        if (me.type == "m.room.encrypted") me.encrypted = true;
                        if (ev.contains("content") && ev["content"].is_object()) {
                            const auto& c = ev["content"];
                            if (c.contains("body")) me.body = c["body"].get<std::string>();
                            if (c.contains("msgtype")) me.msgtype = c["msgtype"].get<std::string>();
                            if (c.contains("format")) me.format = c["format"].get<std::string>();
                            if (c.contains("formatted_body")) {
                                me.formatted_body = c["formatted_body"].get<std::string>();
                            }
                            if (c.contains("url")) me.url = c["url"].get<std::string>();
                            if (c.contains("filename")) me.filename = c["filename"].get<std::string>();
                            if (c.contains("membership")) {
                                me.membership = c["membership"].get<std::string>();
                            }
                            if (c.contains("name")) me.room_name = c["name"].get<std::string>();
                            if (c.contains("topic")) me.room_topic = c["topic"].get<std::string>();
                            if (c.contains("m.relates_to") && c["m.relates_to"].is_object()) {
                                const auto& rel = c["m.relates_to"];
                                if (rel.contains("rel_type")) {
                                    me.relates_to_rel_type = rel["rel_type"].get<std::string>();
                                }
                                if (rel.contains("event_id")) {
                                    me.relates_to_event_id = rel["event_id"].get<std::string>();
                                }
                                if (rel.contains("key")) {
                                    me.reaction_key = rel["key"].get<std::string>();
                                }
                                if (rel.contains("m.in_reply_to") &&
                                    rel["m.in_reply_to"].is_object() &&
                                    rel["m.in_reply_to"].contains("event_id")) {
                                    me.in_reply_to_event_id =
                                        rel["m.in_reply_to"]["event_id"].get<std::string>();
                                }
                            }
                        }
                        evs.push_back(std::move(me));
                    }
                }
                if (!evs.empty()) r.room_events[room_id] = std::move(evs);

                // Ephemeral events: typing.
                if (room_body.contains("ephemeral") &&
                    room_body["ephemeral"].contains("events")) {
                    for (const auto& ev : room_body["ephemeral"]["events"]) {
                        if (ev.value("type", std::string{}) == "m.typing" &&
                            ev.contains("content") && ev["content"].contains("user_ids")) {
                            std::vector<std::string> users;
                            for (const auto& u : ev["content"]["user_ids"]) {
                                users.push_back(u.get<std::string>());
                            }
                            r.typing[room_id] = std::move(users);
                        }
                    }
                }
            }
        }
        // Invites.
        if (rooms.contains("invite") && rooms["invite"].is_object()) {
            for (auto it = rooms["invite"].begin(); it != rooms["invite"].end(); ++it) {
                std::vector<MatrixEvent> evs;
                MatrixEvent me;
                me.room_id = it.key();
                me.type = "m.room.member";
                me.membership = "invite";
                evs.push_back(std::move(me));
                r.invites[it.key()] = std::move(evs);
            }
        }
    }

    if (body.contains("presence") && body["presence"].contains("events")) {
        for (const auto& ev : body["presence"]["events"]) {
            if (ev.contains("sender") && ev.contains("content") &&
                ev["content"].contains("presence")) {
                r.presence[ev["sender"].get<std::string>()] =
                    ev["content"]["presence"].get<std::string>();
            }
        }
    }
    return r;
}

// ── Helpers ──────────────────────────────────────────────────────────────

bool MatrixAdapter::is_bot_mentioned(const std::string& body,
                                     const std::string& formatted_body) const {
    if (body.empty() && formatted_body.empty()) return false;
    if (!cfg_.user_id.empty() && body.find(cfg_.user_id) != std::string::npos) return true;
    if (!cfg_.user_id.empty()) {
        auto colon = cfg_.user_id.find(':');
        if (colon != std::string::npos) {
            std::string localpart = cfg_.user_id.substr(0, colon);
            if (!localpart.empty() && localpart.front() == '@') localpart.erase(0, 1);
            if (!localpart.empty()) {
                std::string pattern = "\\b" + localpart + "\\b";
                try {
                    std::regex re(pattern, std::regex::icase);
                    if (std::regex_search(body, re)) return true;
                } catch (...) {}
            }
        }
    }
    if (!formatted_body.empty() && !cfg_.user_id.empty()) {
        std::string needle = "matrix.to/#/" + cfg_.user_id;
        if (formatted_body.find(needle) != std::string::npos) return true;
    }
    return false;
}

std::string MatrixAdapter::strip_mention(const std::string& body) const {
    std::string out = body;
    if (!cfg_.user_id.empty()) {
        auto pos = out.find(cfg_.user_id);
        while (pos != std::string::npos) {
            out.erase(pos, cfg_.user_id.size());
            pos = out.find(cfg_.user_id);
        }
        auto colon = cfg_.user_id.find(':');
        if (colon != std::string::npos) {
            std::string localpart = cfg_.user_id.substr(0, colon);
            if (!localpart.empty() && localpart.front() == '@') localpart.erase(0, 1);
            if (!localpart.empty()) {
                std::string pattern = "\\b" + localpart + "\\b";
                try {
                    std::regex re(pattern, std::regex::icase);
                    out = std::regex_replace(out, re, "");
                } catch (...) {}
            }
        }
    }
    return strip_ws(out);
}

std::string MatrixAdapter::html_escape(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        switch (c) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '"': out += "&quot;"; break;
            case '\'': out += "&#39;"; break;
            default: out.push_back(c); break;
        }
    }
    return out;
}

std::string MatrixAdapter::sanitize_link_url(const std::string& url) {
    std::string stripped = strip_ws(url);
    auto colon = stripped.find(':');
    if (colon != std::string::npos) {
        std::string scheme = to_lower(strip_ws(stripped.substr(0, colon)));
        if (scheme == "javascript" || scheme == "data" || scheme == "vbscript") {
            return {};
        }
    }
    // Escape double quotes for href.
    std::string out;
    out.reserve(stripped.size());
    for (char c : stripped) {
        if (c == '"') out += "&quot;";
        else out.push_back(c);
    }
    return out;
}

// Markdown-to-HTML: regex-based, mirrors _markdown_to_html_fallback.
// We intentionally keep this conservative — not a full CommonMark parser, but
// covers the common cases the bot produces (bold / italic / code / links /
// lists / blockquote / headers / fenced code).
std::string MatrixAdapter::markdown_to_html(const std::string& text) {
    if (text.empty()) return text;

    struct Placeholder { std::string marker; std::string fragment; };
    std::vector<Placeholder> placeholders;
    auto protect = [&](const std::string& fragment) -> std::string {
        std::string marker = "\x01PROTECT" + std::to_string(placeholders.size()) + "\x02";
        placeholders.push_back({marker, fragment});
        return marker;
    };

    std::string result = text;

    // Fenced code blocks ```lang\n...\n```
    try {
        std::regex fenced("```([A-Za-z0-9_+-]*)\\n([\\s\\S]*?)```");
        std::smatch m;
        std::string scan = result;
        std::string out;
        auto begin = scan.cbegin();
        while (std::regex_search(begin, scan.cend(), m, fenced)) {
            out.append(begin, m[0].first);
            std::string lang = m[1].str();
            std::string code = m[2].str();
            std::string frag = lang.empty()
                ? "<pre><code>" + html_escape(code) + "</code></pre>"
                : "<pre><code class=\"language-" + html_escape(lang) + "\">" +
                      html_escape(code) + "</code></pre>";
            out += protect(frag);
            begin = m[0].second;
        }
        out.append(begin, scan.cend());
        result = out;
    } catch (...) {}

    // Inline code `code`
    try {
        std::regex inline_re("`([^`\\n]+)`");
        std::smatch m;
        std::string scan = result;
        std::string out;
        auto begin = scan.cbegin();
        while (std::regex_search(begin, scan.cend(), m, inline_re)) {
            out.append(begin, m[0].first);
            out += protect("<code>" + html_escape(m[1].str()) + "</code>");
            begin = m[0].second;
        }
        out.append(begin, scan.cend());
        result = out;
    } catch (...) {}

    // Markdown links [text](url)
    try {
        std::regex link_re("\\[([^\\]]+)\\]\\(([^)]+)\\)");
        std::smatch m;
        std::string scan = result;
        std::string out;
        auto begin = scan.cbegin();
        while (std::regex_search(begin, scan.cend(), m, link_re)) {
            out.append(begin, m[0].first);
            std::string href = sanitize_link_url(m[2].str());
            out += protect("<a href=\"" + href + "\">" + html_escape(m[1].str()) + "</a>");
            begin = m[0].second;
        }
        out.append(begin, scan.cend());
        result = out;
    } catch (...) {}

    // HTML-escape the remainder, skipping protected markers.
    {
        std::string out;
        std::size_t i = 0;
        while (i < result.size()) {
            if (result[i] == '\x01') {
                // find matching \x02
                auto end = result.find('\x02', i);
                if (end == std::string::npos) { out.push_back(result[i++]); continue; }
                out.append(result, i, end - i + 1);
                i = end + 1;
            } else {
                char c = result[i++];
                switch (c) {
                    case '&': out += "&amp;"; break;
                    case '<': out += "&lt;"; break;
                    case '>': out += "&gt;"; break;
                    case '"': out += "&quot;"; break;
                    case '\'': out += "&#39;"; break;
                    default: out.push_back(c); break;
                }
            }
        }
        result = out;
    }

    // Block-level: headers, blockquote, unordered/ordered lists, HR.
    std::vector<std::string> lines;
    {
        std::string cur;
        for (char c : result) {
            if (c == '\n') { lines.push_back(cur); cur.clear(); }
            else cur.push_back(c);
        }
        lines.push_back(cur);
    }
    std::vector<std::string> out_lines;
    try {
        std::regex hr("^\\s*([-*_])\\s*\\1\\s*\\1[\\s\\-*_]*$");
        std::regex hdr("^(#{1,6})\\s+(.+)$");
        std::regex ul("^\\s*[-*+]\\s+(.+)$");
        std::regex ol("^\\s*\\d+[.)]\\s+(.+)$");
        for (std::size_t i = 0; i < lines.size();) {
            const auto& line = lines[i];
            std::smatch m;
            if (std::regex_match(line, m, hr)) {
                out_lines.push_back("<hr>");
                ++i; continue;
            }
            if (std::regex_match(line, m, hdr)) {
                int level = static_cast<int>(m[1].str().size());
                out_lines.push_back("<h" + std::to_string(level) + ">" +
                                    strip_ws(m[2].str()) + "</h" +
                                    std::to_string(level) + ">");
                ++i; continue;
            }
            if (starts_with(line, "&gt; ") || line == "&gt;" ||
                starts_with(line, "> ") || line == ">") {
                std::string bq;
                while (i < lines.size() &&
                       (starts_with(lines[i], "&gt; ") || lines[i] == "&gt;" ||
                        starts_with(lines[i], "> ") || lines[i] == ">")) {
                    const auto& ln = lines[i];
                    if (starts_with(ln, "&gt; ")) bq += ln.substr(5);
                    else if (starts_with(ln, "> ")) bq += ln.substr(2);
                    if (i + 1 < lines.size()) bq += "<br>";
                    ++i;
                }
                out_lines.push_back("<blockquote>" + bq + "</blockquote>");
                continue;
            }
            if (std::regex_match(line, m, ul)) {
                std::string items;
                while (i < lines.size() && std::regex_match(lines[i], m, ul)) {
                    items += "<li>" + m[1].str() + "</li>";
                    ++i;
                }
                out_lines.push_back("<ul>" + items + "</ul>");
                continue;
            }
            if (std::regex_match(line, m, ol)) {
                std::string items;
                while (i < lines.size() && std::regex_match(lines[i], m, ol)) {
                    items += "<li>" + m[1].str() + "</li>";
                    ++i;
                }
                out_lines.push_back("<ol>" + items + "</ol>");
                continue;
            }
            out_lines.push_back(line);
            ++i;
        }
    } catch (...) {
        out_lines = lines;
    }
    result.clear();
    for (std::size_t i = 0; i < out_lines.size(); ++i) {
        result += out_lines[i];
        if (i + 1 < out_lines.size()) result += "\n";
    }

    // Inline transforms: bold, italic, strikethrough.
    try {
        result = std::regex_replace(result, std::regex("\\*\\*([^*]+)\\*\\*"), "<strong>$1</strong>");
        result = std::regex_replace(result, std::regex("__([^_]+)__"), "<strong>$1</strong>");
        result = std::regex_replace(result, std::regex("\\*([^*]+)\\*"), "<em>$1</em>");
        result = std::regex_replace(result, std::regex("~~([^~]+)~~"), "<del>$1</del>");
    } catch (...) {}

    // Restore protected regions.
    for (const auto& p : placeholders) {
        std::size_t pos = 0;
        while ((pos = result.find(p.marker, pos)) != std::string::npos) {
            result.replace(pos, p.marker.size(), p.fragment);
            pos += p.fragment.size();
        }
    }
    return result;
}

bool MatrixAdapter::observe_event(const std::string& event_id) {
    if (event_id.empty()) return true;  // not recorded, but don't block
    if (recent_event_id_set_.count(event_id)) return false;
    recent_event_ids_.push_back(event_id);
    recent_event_id_set_.insert(event_id);
    while (recent_event_ids_.size() > kMaxRecentEvents) {
        recent_event_id_set_.erase(recent_event_ids_.front());
        recent_event_ids_.pop_front();
    }
    return true;
}

void MatrixAdapter::track_thread(const std::string& thread_id) {
    if (thread_id.empty()) return;
    participated_threads_.insert(thread_id);
}

bool MatrixAdapter::is_thread_participated(const std::string& thread_id) const {
    return participated_threads_.count(thread_id) > 0;
}

bool MatrixAdapter::load_participated_threads() {
    std::ifstream in(participated_threads_path());
    if (!in) return false;
    std::string line;
    while (std::getline(in, line)) {
        line = strip_ws(line);
        if (!line.empty()) participated_threads_.insert(line);
    }
    return true;
}

bool MatrixAdapter::save_participated_threads() const {
    std::ofstream out(participated_threads_path(),
                      std::ios::trunc);
    if (!out) return false;
    for (const auto& t : participated_threads_) out << t << "\n";
    return true;
}

bool MatrixAdapter::save_sync_token() const {
    if (next_batch_.empty()) return false;
    std::ofstream out(sync_token_path(), std::ios::trunc);
    if (!out) return false;
    out << next_batch_;
    return true;
}

bool MatrixAdapter::load_sync_token() {
    std::ifstream in(sync_token_path());
    if (!in) return false;
    std::stringstream ss;
    ss << in.rdbuf();
    auto tok = strip_ws(ss.str());
    if (tok.empty()) return false;
    next_batch_ = tok;
    return true;
}

std::string MatrixAdapter::next_txn_id() {
    auto n = ++txn_counter_;
    auto g = ++g_global_txn_counter;
    auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::system_clock::now().time_since_epoch())
                   .count();
    std::ostringstream oss;
    oss << "hermes-" << now << "-" << n << "-" << g;
    return oss.str();
}

// ── E2EE ─────────────────────────────────────────────────────────────────

bool MatrixAdapter::setup_e2ee() {
#ifndef HERMES_GATEWAY_HAS_OLM
    return true;
#else
    if (!olm_account_.available()) return true;

    std::string path = olm_pickle_path();
    std::ifstream in(path, std::ios::binary);
    if (in) {
        std::stringstream ss;
        ss << in.rdbuf();
        auto pickled = ss.str();
        if (!pickled.empty()) {
            (void)olm_account_.unpickle(pickled, cfg_.pickle_passphrase);
        }
    } else {
        auto pickled = olm_account_.pickle(cfg_.pickle_passphrase);
        if (!pickled.empty()) {
            std::ofstream out(path, std::ios::binary | std::ios::trunc);
            if (out) out.write(pickled.data(), static_cast<std::streamsize>(pickled.size()));
        }
    }

    (void)olm_account_.generate_one_time_keys(50);

    auto* transport = get_transport();
    if (transport && !access_token_.empty()) {
        try {
            json device_keys = json::parse(
                olm_account_.identity_keys_json().empty()
                    ? std::string("{}")
                    : olm_account_.identity_keys_json());
            json otks = json::parse(
                olm_account_.one_time_keys_json().empty()
                    ? std::string("{}")
                    : olm_account_.one_time_keys_json());
            json payload = {
                {"device_keys", device_keys},
                {"one_time_keys", otks},
            };
            auto resp = authed_post(v3_url("keys/upload"), payload.dump());
            if (resp.status_code >= 200 && resp.status_code < 300) {
                olm_account_.mark_keys_as_published();
            }
        } catch (...) {
            // Best-effort.
        }
    }
    return true;
#endif
}

bool MatrixAdapter::encrypt_room_message(const std::string& room_id,
                                         const std::string& plaintext,
                                         std::string& out_ciphertext) {
#ifndef HERMES_GATEWAY_HAS_OLM
    (void)room_id;
    out_ciphertext = plaintext;
    return true;
#else
    auto it = megolm_out_.find(room_id);
    if (it == megolm_out_.end() || !it->second.available()) {
        out_ciphertext = plaintext;
        return true;
    }
    auto ct = it->second.encrypt(plaintext);
    if (ct.empty()) return false;
    out_ciphertext = std::move(ct);
    return true;
#endif
}

std::optional<std::string> MatrixAdapter::decrypt_room_message(
    const std::string& room_id, const std::string& ciphertext) {
#ifndef HERMES_GATEWAY_HAS_OLM
    (void)room_id;
    return ciphertext;
#else
    auto room_it = megolm_in_.find(room_id);
    if (room_it == megolm_in_.end()) return ciphertext;
    for (auto& [sid, session] : room_it->second) {
        auto pt = session.decrypt(ciphertext);
        if (pt.has_value()) return pt;
    }
    return std::nullopt;
#endif
}

std::string MatrixAdapter::claim_one_time_keys(
    const std::vector<std::pair<std::string, std::string>>& user_devices) {
    if (access_token_.empty() || user_devices.empty()) return {};
    json one_time = json::object();
    for (const auto& [uid, did] : user_devices) {
        if (!one_time.contains(uid)) one_time[uid] = json::object();
        one_time[uid][did] = "signed_curve25519";
    }
    json payload = {{"one_time_keys", one_time}, {"timeout", 10000}};
    try {
        auto resp = authed_post(v3_url("keys/claim"), payload.dump());
        if (resp.status_code < 200 || resp.status_code >= 300) return {};
        return resp.body;
    } catch (...) {
        return {};
    }
}

std::string MatrixAdapter::query_device_keys(
    const std::vector<std::string>& user_ids) {
    if (access_token_.empty() || user_ids.empty()) return {};
    json device_keys = json::object();
    for (const auto& uid : user_ids) {
        device_keys[uid] = json::array();
    }
    json payload = {{"device_keys", device_keys}, {"timeout", 10000}};
    try {
        auto resp = authed_post(v3_url("keys/query"), payload.dump());
        if (resp.status_code < 200 || resp.status_code >= 300) return {};
        return resp.body;
    } catch (...) {
        return {};
    }
}

bool MatrixAdapter::upload_cross_signing_keys(const std::string& master_json,
                                              const std::string& self_signing_json,
                                              const std::string& user_signing_json) {
    if (access_token_.empty()) return false;
    json payload = json::object();
    auto try_put = [&](const std::string& key, const std::string& body) {
        if (body.empty()) return;
        auto parsed = json::parse(body, nullptr, false);
        if (!parsed.is_discarded()) payload[key] = parsed;
    };
    try_put("master_key", master_json);
    try_put("self_signing_key", self_signing_json);
    try_put("user_signing_key", user_signing_json);
    if (payload.empty()) return false;
    try {
        auto resp = authed_post(v3_url("keys/device_signing/upload"), payload.dump());
        return resp.status_code >= 200 && resp.status_code < 300;
    } catch (...) {
        return false;
    }
}

bool MatrixAdapter::enable_room_encryption(const std::string& room_id) {
    if (access_token_.empty()) return false;
    json payload = {
        {"algorithm", "m.megolm.v1.aes-sha2"},
        {"rotation_period_ms", 604800000},
        {"rotation_period_msgs", 100},
    };
    try {
        auto resp = authed_post(v3_url("rooms/" + url_encode(room_id) +
                                       "/state/m.room.encryption/"),
                                payload.dump());
        if (resp.status_code >= 200 && resp.status_code < 300) {
            encrypted_rooms_.insert(room_id);
            return true;
        }
        return false;
    } catch (...) {
        return false;
    }
}

std::string MatrixAdapter::ensure_outbound_megolm(const std::string& room_id) {
#ifndef HERMES_GATEWAY_HAS_OLM
    (void)room_id;
    return {};
#else
    auto it = megolm_out_.find(room_id);
    if (it != megolm_out_.end() && it->second.available()) {
        return it->second.session_id();
    }
    MegolmOutboundSession session;
    if (!session.available()) return {};
    auto sid = session.session_id();
    megolm_out_.emplace(room_id, std::move(session));
    return sid;
#endif
}

bool MatrixAdapter::import_inbound_megolm(const std::string& room_id,
                                          const std::string& session_key) {
#ifndef HERMES_GATEWAY_HAS_OLM
    (void)room_id; (void)session_key;
    return false;
#else
    MegolmInboundSession in;
    if (!in.init_from_session_key(session_key)) return false;
    auto sid = in.session_id();
    megolm_in_[room_id][sid] = std::move(in);
    return true;
#endif
}

}  // namespace hermes::gateway::platforms
