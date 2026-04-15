// Phase 12 — OpenAI-compatible API Server adapter implementation (depth port).
#include "api_server.hpp"

#include <algorithm>
#include <cctype>
#include <iomanip>
#include <sstream>
#include <unordered_set>

#include <openssl/hmac.h>
#include <openssl/sha.h>

namespace hermes::gateway::platforms {

namespace {

std::string to_lower(std::string s) {
    for (auto& c : s) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return s;
}

std::string trim(std::string s) {
    auto not_space = [](unsigned char c) { return std::isspace(c) == 0; };
    s.erase(s.begin(), std::find_if(s.begin(), s.end(), not_space));
    s.erase(std::find_if(s.rbegin(), s.rend(), not_space).base(), s.end());
    return s;
}

std::string sha256_hex(const std::string& data) {
    unsigned char digest[SHA256_DIGEST_LENGTH];
    SHA256(reinterpret_cast<const unsigned char*>(data.data()), data.size(),
           digest);
    std::ostringstream oss;
    oss << std::hex << std::setfill('0');
    for (unsigned char b : digest) {
        oss << std::setw(2) << static_cast<int>(b);
    }
    return oss.str();
}

bool constant_time_eq(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    unsigned char diff = 0;
    for (std::size_t i = 0; i < a.size(); ++i) {
        diff |= static_cast<unsigned char>(a[i]) ^ static_cast<unsigned char>(b[i]);
    }
    return diff == 0;
}

}  // namespace

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

nlohmann::json api_openai_error(const std::string& message,
                                const std::string& err_type,
                                const std::string& param,
                                const std::string& code) {
    nlohmann::json err = {
        {"message", message},
        {"type", err_type},
    };
    if (!param.empty()) err["param"] = param;
    else err["param"] = nullptr;
    if (!code.empty()) err["code"] = code;
    else err["code"] = nullptr;
    return nlohmann::json{{"error", err}};
}

std::string api_derive_chat_session_id(const std::string& system_prompt,
                                       const std::string& first_user_message) {
    std::string seed = system_prompt + "\n" + first_user_message;
    auto digest = sha256_hex(seed);
    return "api-" + digest.substr(0, 16);
}

std::string api_make_request_fingerprint(const nlohmann::json& body,
                                         const std::vector<std::string>& keys) {
    nlohmann::json subset = nlohmann::json::object();
    for (const auto& k : keys) {
        subset[k] = body.contains(k) ? body[k] : nlohmann::json{};
    }
    // Sort keys for deterministic encoding (matches Python repr-of-dict order
    // closely enough for fingerprinting purposes).
    return sha256_hex(subset.dump());
}

bool api_is_valid_job_id(const std::string& job_id) {
    if (job_id.size() != 12) return false;
    for (char c : job_id) {
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    }
    return true;
}

const std::vector<std::string>& api_update_allowed_fields() {
    static const std::vector<std::string> kFields{
        "name", "schedule", "prompt", "deliver",
        "skills", "skill", "repeat", "enabled",
    };
    return kFields;
}

std::string api_resolve_model_name(const std::string& explicit_name,
                                   const std::string& profile_name) {
    std::string e = trim(explicit_name);
    if (!e.empty()) return e;
    std::string p = trim(profile_name);
    if (!p.empty() && p != "default" && p != "custom") return p;
    return "hermes-agent";
}

std::vector<std::string> api_parse_cors_origins(const std::string& raw) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : raw) {
        if (c == ',') {
            std::string t = trim(cur);
            if (!t.empty()) out.push_back(t);
            cur.clear();
        } else {
            cur.push_back(c);
        }
    }
    std::string t = trim(cur);
    if (!t.empty()) out.push_back(t);
    return out;
}

std::optional<std::unordered_map<std::string, std::string>>
api_cors_headers_for_origin(const std::vector<std::string>& configured,
                            const std::string& origin) {
    if (origin.empty() || configured.empty()) return std::nullopt;

    std::unordered_map<std::string, std::string> headers{
        {"Access-Control-Allow-Methods", "GET, POST, DELETE, OPTIONS"},
        {"Access-Control-Allow-Headers",
         "Authorization, Content-Type, Idempotency-Key"},
        {"Access-Control-Max-Age", "600"},
    };

    bool wildcard = std::find(configured.begin(), configured.end(), "*") !=
                    configured.end();
    if (wildcard) {
        headers["Access-Control-Allow-Origin"] = "*";
        return headers;
    }

    if (std::find(configured.begin(), configured.end(), origin) ==
        configured.end()) {
        return std::nullopt;
    }
    headers["Access-Control-Allow-Origin"] = origin;
    headers["Vary"] = "Origin";
    return headers;
}

bool api_origin_allowed(const std::vector<std::string>& configured,
                        const std::string& origin) {
    if (origin.empty()) return true;
    if (configured.empty()) return false;
    if (std::find(configured.begin(), configured.end(), "*") != configured.end()) {
        return true;
    }
    return std::find(configured.begin(), configured.end(), origin) !=
           configured.end();
}

bool api_is_valid_session_id(const std::string& session_id) {
    if (session_id.empty()) return false;
    for (char c : session_id) {
        if (c == '\r' || c == '\n' || c == '\0') return false;
    }
    return true;
}

int api_check_body_size(const std::string& content_length_header) {
    if (content_length_header.empty()) return 0;
    try {
        auto n = std::stoll(content_length_header);
        if (n < 0) return 400;
        if (static_cast<std::size_t>(n) > kApiServerMaxRequestBytes) return 413;
        return 0;
    } catch (...) {
        return 400;
    }
}

ChatCompletionInput api_parse_chat_completion_messages(
    const nlohmann::json& messages) {
    ChatCompletionInput out;
    if (!messages.is_array()) {
        out.error = "Missing or invalid 'messages' field";
        return out;
    }
    std::vector<nlohmann::json> conv;
    for (const auto& msg : messages) {
        if (!msg.is_object()) continue;
        std::string role = msg.value("role", "");
        std::string content;
        if (msg.contains("content") && msg["content"].is_string()) {
            content = msg["content"].get<std::string>();
        }
        if (role == "system") {
            if (out.system_prompt.empty()) out.system_prompt = content;
            else out.system_prompt += "\n" + content;
        } else if (role == "user" || role == "assistant") {
            conv.push_back({{"role", role}, {"content", content}});
        }
    }
    if (conv.empty()) {
        out.error = "No user message found in messages";
        return out;
    }
    out.user_message = conv.back().value("content", "");
    if (out.user_message.empty()) {
        out.error = "No user message found in messages";
        return out;
    }
    out.history.assign(conv.begin(), conv.end() - 1);
    return out;
}

nlohmann::json api_build_chat_completion_response(
    const std::string& completion_id, const std::string& model,
    std::int64_t created, const std::string& final_response,
    std::int64_t input_tokens, std::int64_t output_tokens) {
    return nlohmann::json{
        {"id", completion_id},
        {"object", "chat.completion"},
        {"created", created},
        {"model", model},
        {"choices",
         nlohmann::json::array(
             {{{"index", 0},
               {"message",
                {{"role", "assistant"}, {"content", final_response}}},
               {"finish_reason", "stop"}}})},
        {"usage",
         {{"prompt_tokens", input_tokens},
          {"completion_tokens", output_tokens},
          {"total_tokens", input_tokens + output_tokens}}},
    };
}

nlohmann::json api_build_chat_completion_finish_chunk(
    const std::string& completion_id, const std::string& model,
    std::int64_t created, std::int64_t input_tokens,
    std::int64_t output_tokens) {
    return nlohmann::json{
        {"id", completion_id},
        {"object", "chat.completion.chunk"},
        {"created", created},
        {"model", model},
        {"choices",
         nlohmann::json::array(
             {{{"index", 0}, {"delta", nlohmann::json::object()},
               {"finish_reason", "stop"}}})},
        {"usage",
         {{"prompt_tokens", input_tokens},
          {"completion_tokens", output_tokens},
          {"total_tokens", input_tokens + output_tokens}}},
    };
}

nlohmann::json api_build_response_object(
    const std::string& response_id, const std::string& model,
    std::int64_t created_at, const nlohmann::json& output_items,
    std::int64_t input_tokens, std::int64_t output_tokens) {
    return nlohmann::json{
        {"id", response_id},
        {"object", "response"},
        {"status", "completed"},
        {"created_at", created_at},
        {"model", model},
        {"output", output_items},
        {"usage",
         {{"input_tokens", input_tokens},
          {"output_tokens", output_tokens},
          {"total_tokens", input_tokens + output_tokens}}},
    };
}

bool api_check_bearer_auth(const std::string& configured_key,
                           const std::string& auth_header) {
    if (configured_key.empty()) return true;
    static const std::string kPrefix = "Bearer ";
    if (auth_header.size() <= kPrefix.size()) return false;
    if (auth_header.compare(0, kPrefix.size(), kPrefix) != 0) return false;
    std::string token = trim(auth_header.substr(kPrefix.size()));
    return constant_time_eq(token, configured_key);
}

// ---------------------------------------------------------------------------
// ResponseStore
// ---------------------------------------------------------------------------

ResponseStore::ResponseStore(std::size_t max_size) : max_size_(max_size) {}

std::optional<nlohmann::json> ResponseStore::get(const std::string& response_id) {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = store_.find(response_id);
    if (it == store_.end()) return std::nullopt;
    // Refresh LRU position.
    lru_.erase(it->second.second);
    lru_.push_front(response_id);
    it->second.second = lru_.begin();
    return it->second.first;
}

void ResponseStore::put(const std::string& response_id, nlohmann::json data) {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = store_.find(response_id);
    if (it != store_.end()) {
        lru_.erase(it->second.second);
        lru_.push_front(response_id);
        it->second.first = std::move(data);
        it->second.second = lru_.begin();
    } else {
        lru_.push_front(response_id);
        store_[response_id] = {std::move(data), lru_.begin()};
    }
    while (store_.size() > max_size_) {
        const std::string& oldest = lru_.back();
        store_.erase(oldest);
        lru_.pop_back();
    }
}

bool ResponseStore::remove(const std::string& response_id) {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = store_.find(response_id);
    if (it == store_.end()) return false;
    lru_.erase(it->second.second);
    store_.erase(it);
    return true;
}

std::optional<std::string> ResponseStore::get_conversation(
    const std::string& name) const {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = conversations_.find(name);
    if (it == conversations_.end()) return std::nullopt;
    return it->second;
}

void ResponseStore::set_conversation(const std::string& name,
                                     const std::string& response_id) {
    std::lock_guard<std::mutex> lock(mu_);
    conversations_[name] = response_id;
}

std::size_t ResponseStore::size() const {
    std::lock_guard<std::mutex> lock(mu_);
    return store_.size();
}

// ---------------------------------------------------------------------------
// IdempotencyCache
// ---------------------------------------------------------------------------

IdempotencyCache::IdempotencyCache(std::size_t max_items,
                                   std::chrono::seconds ttl)
    : max_items_(max_items), ttl_(ttl) {}

void IdempotencyCache::purge_locked() {
    auto now = Clock::now();
    for (auto it = lru_.begin(); it != lru_.end();) {
        auto sit = store_.find(*it);
        if (sit == store_.end()) {
            it = lru_.erase(it);
            continue;
        }
        if (now - sit->second.first.ts > ttl_) {
            store_.erase(sit);
            it = lru_.erase(it);
        } else {
            ++it;
        }
    }
    while (store_.size() > max_items_) {
        const std::string& oldest = lru_.back();
        store_.erase(oldest);
        lru_.pop_back();
    }
}

std::optional<nlohmann::json> IdempotencyCache::get(
    const std::string& key, const std::string& fingerprint) {
    std::lock_guard<std::mutex> lock(mu_);
    purge_locked();
    auto it = store_.find(key);
    if (it == store_.end()) return std::nullopt;
    if (it->second.first.fingerprint != fingerprint) return std::nullopt;
    return it->second.first.response;
}

void IdempotencyCache::put(const std::string& key, const std::string& fingerprint,
                           nlohmann::json response) {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = store_.find(key);
    if (it != store_.end()) {
        lru_.erase(it->second.second);
        store_.erase(it);
    }
    lru_.push_front(key);
    Entry entry{std::move(response), fingerprint, Clock::now()};
    store_[key] = {std::move(entry), lru_.begin()};
    purge_locked();
}

std::size_t IdempotencyCache::size() const {
    std::lock_guard<std::mutex> lock(mu_);
    return store_.size();
}

// ---------------------------------------------------------------------------
// Adapter
// ---------------------------------------------------------------------------

ApiServerAdapter::ApiServerAdapter(Config cfg) : cfg_(std::move(cfg)) {}

bool ApiServerAdapter::connect() {
    connected_ = true;
    return true;
}

void ApiServerAdapter::disconnect() {
    connected_ = false;
}

bool ApiServerAdapter::send(const std::string& chat_id,
                            const std::string& content) {
    std::lock_guard<std::mutex> lock(queue_mu_);
    pending_responses_[chat_id] = content;
    return true;
}

std::string ApiServerAdapter::get_pending_response(const std::string& chat_id) {
    std::lock_guard<std::mutex> lock(queue_mu_);
    auto it = pending_responses_.find(chat_id);
    if (it == pending_responses_.end()) return {};
    std::string out = std::move(it->second);
    pending_responses_.erase(it);
    return out;
}

bool ApiServerAdapter::verify_hmac_signature(const std::string& secret,
                                             const std::string& body,
                                             const std::string& signature_hex) {
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int digest_len = 0;
    HMAC(EVP_sha256(), secret.data(), static_cast<int>(secret.size()),
         reinterpret_cast<const unsigned char*>(body.data()), body.size(),
         digest, &digest_len);
    std::ostringstream oss;
    oss << std::hex << std::setfill('0');
    for (unsigned int i = 0; i < digest_len; ++i) {
        oss << std::setw(2) << static_cast<int>(digest[i]);
    }
    return constant_time_eq(oss.str(), to_lower(signature_hex));
}

bool ApiServerAdapter::check_auth(const std::string& auth_header) const {
    return api_check_bearer_auth(cfg_.api_key, auth_header);
}

bool ApiServerAdapter::origin_allowed(const std::string& origin) const {
    return api_origin_allowed(cfg_.cors_origins, origin);
}

std::optional<std::unordered_map<std::string, std::string>>
ApiServerAdapter::cors_headers_for_origin(const std::string& origin) const {
    return api_cors_headers_for_origin(cfg_.cors_origins, origin);
}

std::string ApiServerAdapter::resolved_model_name() const {
    return api_resolve_model_name(cfg_.model_name, cfg_.profile_name);
}

}  // namespace hermes::gateway::platforms
