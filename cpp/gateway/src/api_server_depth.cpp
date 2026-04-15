// Implementation of the gateway API-server pure helpers.

#include <hermes/gateway/api_server_depth.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <iomanip>
#include <sstream>

#ifdef HERMES_HAVE_OPENSSL
#include <openssl/sha.h>
#endif

namespace hermes::gateway::api_server_depth {

namespace {

std::string trim(std::string_view s) {
    auto begin = std::size_t{0};
    auto end = s.size();
    while (begin < end && std::isspace(static_cast<unsigned char>(s[begin]))) {
        ++begin;
    }
    while (end > begin && std::isspace(static_cast<unsigned char>(s[end - 1]))) {
        --end;
    }
    return std::string{s.substr(begin, end - begin)};
}

std::string hex_sha256(std::string_view data) {
#ifdef HERMES_HAVE_OPENSSL
    std::array<unsigned char, SHA256_DIGEST_LENGTH> buf{};
    SHA256(reinterpret_cast<const unsigned char*>(data.data()), data.size(),
           buf.data());
    std::ostringstream oss;
    oss << std::hex << std::setfill('0');
    for (auto b : buf) {
        oss << std::setw(2) << static_cast<int>(b);
    }
    return oss.str();
#else
    // Deterministic non-cryptographic fallback — only used when the
    // build is linked without OpenSSL (host-side tooling).  The
    // production gateway always links OpenSSL.
    std::uint64_t h{1469598103934665603ULL};  // FNV-1a 64-bit offset
    for (auto c : data) {
        h ^= static_cast<unsigned char>(c);
        h *= 1099511628211ULL;
    }
    std::array<char, 17> out{};
    std::snprintf(out.data(), out.size(), "%016llx",
                   static_cast<unsigned long long>(h));
    std::string digest{out.data()};
    digest.append(48, '0');  // pad to 64 hex chars
    return digest;
#endif
}

}  // namespace

// --- OpenAI error envelope ---------------------------------------------

nlohmann::json openai_error(std::string_view message,
                             std::string_view err_type,
                             std::string_view param,
                             std::string_view code) {
    nlohmann::json body;
    body["message"] = std::string{message};
    body["type"] = err_type.empty() ? std::string{"invalid_request_error"}
                                     : std::string{err_type};
    if (param.empty()) {
        body["param"] = nullptr;
    } else {
        body["param"] = std::string{param};
    }
    if (code.empty()) {
        body["code"] = nullptr;
    } else {
        body["code"] = std::string{code};
    }
    nlohmann::json out;
    out["error"] = std::move(body);
    return out;
}

// --- Request fingerprinting --------------------------------------------

std::string make_request_fingerprint(const nlohmann::json& body,
                                       const std::vector<std::string>& keys) {
    // We serialize a deterministic JSON object so two request bodies
    // that differ only in key ordering hash identically.  Python uses
    // ``repr()`` of a dict built with a fixed ``keys`` order — we
    // mirror that ordering explicitly here.
    nlohmann::json ordered = nlohmann::json::object();
    for (const auto& k : keys) {
        if (body.is_object() && body.contains(k)) {
            ordered[k] = body.at(k);
        } else {
            ordered[k] = nullptr;
        }
    }
    // dump with sort_keys=false preserves insertion order from ordered.
    const auto payload = ordered.dump();
    return hex_sha256(payload);
}

// --- Session ID derivation ---------------------------------------------

std::string derive_chat_session_id(std::string_view system_prompt,
                                     std::string_view first_user_message) {
    std::string seed;
    seed.append(system_prompt);
    seed.push_back('\n');
    seed.append(first_user_message);
    const auto digest = hex_sha256(seed);
    return std::string{"api-"} + digest.substr(0, 16);
}

// --- CORS origins -------------------------------------------------------

std::vector<std::string> parse_cors_origins(std::string_view value) {
    std::vector<std::string> out;
    if (value.empty()) {
        return out;
    }
    std::size_t start{0};
    while (start <= value.size()) {
        const auto comma = value.find(',', start);
        const auto end = (comma == std::string_view::npos) ? value.size() : comma;
        auto piece = trim(value.substr(start, end - start));
        if (!piece.empty()) {
            out.push_back(std::move(piece));
        }
        if (comma == std::string_view::npos) {
            break;
        }
        start = comma + 1;
    }
    return out;
}

std::vector<std::string> parse_cors_origins(const nlohmann::json& value) {
    std::vector<std::string> out;
    if (value.is_null()) {
        return out;
    }
    if (value.is_string()) {
        return parse_cors_origins(
            std::string_view{value.get_ref<const std::string&>()});
    }
    if (value.is_array()) {
        for (const auto& item : value) {
            std::string piece;
            if (item.is_string()) {
                piece = trim(item.get_ref<const std::string&>());
            } else {
                piece = trim(item.dump());
            }
            if (!piece.empty()) {
                out.push_back(std::move(piece));
            }
        }
        return out;
    }
    // Fallback: treat as stringified scalar.
    const auto piece = trim(value.dump());
    if (!piece.empty()) {
        out.push_back(piece);
    }
    return out;
}

bool origin_allowed(const std::vector<std::string>& allowed,
                     std::string_view origin) {
    if (origin.empty()) {
        return true;  // non-browser clients
    }
    if (allowed.empty()) {
        return false;
    }
    for (const auto& item : allowed) {
        if (item == "*") {
            return true;
        }
        if (item == origin) {
            return true;
        }
    }
    return false;
}

std::optional<std::map<std::string, std::string>>
cors_headers_for_origin(const std::vector<std::string>& allowed,
                          std::string_view origin) {
    if (origin.empty() || allowed.empty()) {
        return std::nullopt;
    }
    std::map<std::string, std::string> base{
        {"Access-Control-Allow-Methods", "GET, POST, DELETE, OPTIONS"},
        {"Access-Control-Allow-Headers",
         "Authorization, Content-Type, Idempotency-Key"},
    };
    const bool has_wildcard = std::find(allowed.begin(), allowed.end(),
                                          std::string{"*"}) != allowed.end();
    if (has_wildcard) {
        base["Access-Control-Allow-Origin"] = "*";
        base["Access-Control-Max-Age"] = "600";
        return base;
    }
    if (std::find(allowed.begin(), allowed.end(), std::string{origin}) ==
        allowed.end()) {
        return std::nullopt;
    }
    base["Access-Control-Allow-Origin"] = std::string{origin};
    base["Vary"] = "Origin";
    base["Access-Control-Max-Age"] = "600";
    return base;
}

// --- Model name resolution --------------------------------------------

std::string resolve_model_name(std::string_view explicit_name,
                                 std::string_view profile,
                                 std::string_view fallback) {
    const auto explicit_trimmed = trim(explicit_name);
    if (!explicit_trimmed.empty()) {
        return explicit_trimmed;
    }
    const auto profile_trimmed = trim(profile);
    if (!profile_trimmed.empty() && profile_trimmed != "default" &&
        profile_trimmed != "custom") {
        return profile_trimmed;
    }
    return std::string{fallback};
}

// --- Idempotency cache -------------------------------------------------

IdempotencyCache::IdempotencyCache(std::size_t max_items,
                                      std::chrono::seconds ttl, Clock clock)
    : max_{max_items}, ttl_{ttl}, clock_{std::move(clock)} {
    if (max_ == 0) {
        max_ = 1;
    }
}

std::chrono::steady_clock::time_point IdempotencyCache::clock_now() const {
    if (clock_.now) {
        return clock_.now();
    }
    return std::chrono::steady_clock::now();
}

void IdempotencyCache::purge() {
    std::lock_guard<std::mutex> lock{mu_};
    const auto now = clock_now();
    // Drop expired entries first.
    for (auto it = store_.begin(); it != store_.end();) {
        if (now - it->second.ts > ttl_) {
            const auto& key = it->first;
            auto oit =
                std::find(access_order_.begin(), access_order_.end(), key);
            if (oit != access_order_.end()) {
                access_order_.erase(oit);
            }
            it = store_.erase(it);
        } else {
            ++it;
        }
    }
    // Evict oldest while over-capacity.
    while (access_order_.size() > max_) {
        const auto& oldest = access_order_.front();
        store_.erase(oldest);
        access_order_.pop_front();
    }
}

std::optional<nlohmann::json>
IdempotencyCache::peek(const std::string& key, const std::string& fingerprint) {
    purge();
    std::lock_guard<std::mutex> lock{mu_};
    const auto it = store_.find(key);
    if (it == store_.end()) {
        return std::nullopt;
    }
    if (it->second.fingerprint != fingerprint) {
        return std::nullopt;
    }
    return it->second.value;
}

void IdempotencyCache::put(const std::string& key,
                             const std::string& fingerprint,
                             nlohmann::json value) {
    {
        std::lock_guard<std::mutex> lock{mu_};
        const auto existing = store_.find(key);
        if (existing != store_.end()) {
            auto oit =
                std::find(access_order_.begin(), access_order_.end(), key);
            if (oit != access_order_.end()) {
                access_order_.erase(oit);
            }
            store_.erase(existing);
        }
        Entry entry{fingerprint, std::move(value), clock_now()};
        store_.emplace(key, std::move(entry));
        access_order_.push_back(key);
    }
    purge();
}

nlohmann::json IdempotencyCache::get_or_compute(
    const std::string& key, const std::string& fingerprint,
    const std::function<nlohmann::json()>& compute) {
    if (auto cached = peek(key, fingerprint); cached.has_value()) {
        return *cached;
    }
    auto computed = compute ? compute() : nlohmann::json{};
    put(key, fingerprint, computed);
    return computed;
}

std::size_t IdempotencyCache::size() const {
    std::lock_guard<std::mutex> lock{mu_};
    return store_.size();
}

// --- Output items -------------------------------------------------------

nlohmann::json extract_output_items(const nlohmann::json& result) {
    nlohmann::json items = nlohmann::json::array();
    if (result.is_object() && result.contains("messages") &&
        result.at("messages").is_array()) {
        for (const auto& msg : result.at("messages")) {
            if (!msg.is_object()) {
                continue;
            }
            const auto role = msg.value("role", std::string{});
            if (role == "assistant" && msg.contains("tool_calls") &&
                msg.at("tool_calls").is_array()) {
                for (const auto& tc : msg.at("tool_calls")) {
                    if (!tc.is_object()) {
                        continue;
                    }
                    const auto& func = tc.contains("function")
                                             ? tc.at("function")
                                             : nlohmann::json::object();
                    nlohmann::json item{
                        {"type", "function_call"},
                        {"name", func.value("name", std::string{})},
                        {"arguments", func.value("arguments", std::string{})},
                        {"call_id", tc.value("id", std::string{})},
                    };
                    items.push_back(std::move(item));
                }
            } else if (role == "tool") {
                nlohmann::json item{
                    {"type", "function_call_output"},
                    {"call_id", msg.value("tool_call_id", std::string{})},
                    {"output", msg.value("content", std::string{})},
                };
                items.push_back(std::move(item));
            }
        }
    }
    // Final assistant message.
    std::string final_text;
    if (result.is_object()) {
        final_text = result.value("final_response", std::string{});
        if (final_text.empty()) {
            final_text = result.value("error", std::string{"(No response generated)"});
        }
    } else {
        final_text = "(No response generated)";
    }
    nlohmann::json final_msg{
        {"type", "message"},
        {"role", "assistant"},
        {"content",
          nlohmann::json::array({nlohmann::json{{"type", "output_text"},
                                                   {"text", final_text}}})},
    };
    items.push_back(std::move(final_msg));
    return items;
}

// --- Body-limit classification -----------------------------------------

BodyLimitStatus classify_body_length(std::string_view content_length,
                                       std::size_t max_bytes) {
    if (content_length.empty()) {
        return BodyLimitStatus::Ok;
    }
    // Must be pure digits.
    for (auto c : content_length) {
        if (!std::isdigit(static_cast<unsigned char>(c))) {
            return BodyLimitStatus::Invalid;
        }
    }
    try {
        const auto value = std::stoull(std::string{content_length});
        if (value > max_bytes) {
            return BodyLimitStatus::TooLarge;
        }
        return BodyLimitStatus::Ok;
    } catch (const std::exception&) {
        return BodyLimitStatus::Invalid;
    }
}

// --- Bearer token auth --------------------------------------------------

namespace {

bool constant_time_equals(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) {
        return false;
    }
    unsigned char diff{0};
    for (std::size_t i{0}; i < a.size(); ++i) {
        diff |= static_cast<unsigned char>(a[i]) ^
                 static_cast<unsigned char>(b[i]);
    }
    return diff == 0;
}

}  // namespace

bool check_bearer_auth(std::string_view auth_header,
                        std::string_view api_key) {
    if (api_key.empty()) {
        return true;
    }
    static constexpr std::string_view prefix{"Bearer "};
    if (auth_header.size() < prefix.size() ||
        auth_header.substr(0, prefix.size()) != prefix) {
        return false;
    }
    auto token = auth_header.substr(prefix.size());
    // Strip surrounding whitespace.
    while (!token.empty() &&
           std::isspace(static_cast<unsigned char>(token.front()))) {
        token.remove_prefix(1);
    }
    while (!token.empty() &&
           std::isspace(static_cast<unsigned char>(token.back()))) {
        token.remove_suffix(1);
    }
    return constant_time_equals(token, api_key);
}

}  // namespace hermes::gateway::api_server_depth
