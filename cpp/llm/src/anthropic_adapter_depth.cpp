// Anthropic adapter depth port — implementation.
//
// All helpers are pure: no sockets, no file I/O, no subprocess.  Network
// is in anthropic_client.cpp.  Auth file parsing lives in hermes/auth.
#include "hermes/llm/anthropic_adapter_depth.hpp"

#include "hermes/llm/anthropic_features.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <random>
#include <regex>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace hermes::llm {

using nlohmann::json;

namespace {

// ── small helpers ──────────────────────────────────────────────────────

std::string to_lower(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        out.push_back(static_cast<char>(
            std::tolower(static_cast<unsigned char>(c))));
    }
    return out;
}

bool starts_with(std::string_view s, std::string_view prefix) {
    return s.size() >= prefix.size() &&
           s.compare(0, prefix.size(), prefix) == 0;
}

bool starts_with_ci(std::string_view s, std::string_view prefix) {
    if (s.size() < prefix.size()) return false;
    for (std::size_t i = 0; i < prefix.size(); ++i) {
        const char a = static_cast<char>(
            std::tolower(static_cast<unsigned char>(s[i])));
        const char b = static_cast<char>(
            std::tolower(static_cast<unsigned char>(prefix[i])));
        if (a != b) return false;
    }
    return true;
}

bool contains_ci(std::string_view hay, std::string_view needle) {
    if (needle.empty()) return true;
    if (hay.size() < needle.size()) return false;
    for (std::size_t i = 0; i + needle.size() <= hay.size(); ++i) {
        bool ok = true;
        for (std::size_t j = 0; j < needle.size(); ++j) {
            const char a = static_cast<char>(
                std::tolower(static_cast<unsigned char>(hay[i + j])));
            const char b = static_cast<char>(
                std::tolower(static_cast<unsigned char>(needle[j])));
            if (a != b) { ok = false; break; }
        }
        if (ok) return true;
    }
    return false;
}

std::string replace_all(std::string_view s,
                        std::string_view needle,
                        std::string_view repl) {
    if (needle.empty()) return std::string(s);
    std::string out;
    out.reserve(s.size());
    std::size_t i = 0;
    while (i < s.size()) {
        if (i + needle.size() <= s.size() &&
            s.compare(i, needle.size(), needle) == 0) {
            out.append(repl);
            i += needle.size();
        } else {
            out.push_back(s[i++]);
        }
    }
    return out;
}

// URL-safe base64 without padding.
std::string urlsafe_b64(const unsigned char* data, std::size_t len) {
    static const char* kAlphabet =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    std::string out;
    out.reserve(((len + 2) / 3) * 4);
    std::size_t i = 0;
    while (i + 3 <= len) {
        uint32_t triple = (uint32_t(data[i]) << 16) |
                          (uint32_t(data[i + 1]) << 8) | data[i + 2];
        out.push_back(kAlphabet[(triple >> 18) & 0x3F]);
        out.push_back(kAlphabet[(triple >> 12) & 0x3F]);
        out.push_back(kAlphabet[(triple >> 6) & 0x3F]);
        out.push_back(kAlphabet[triple & 0x3F]);
        i += 3;
    }
    const std::size_t rem = len - i;
    if (rem == 1) {
        uint32_t triple = uint32_t(data[i]) << 16;
        out.push_back(kAlphabet[(triple >> 18) & 0x3F]);
        out.push_back(kAlphabet[(triple >> 12) & 0x3F]);
    } else if (rem == 2) {
        uint32_t triple = (uint32_t(data[i]) << 16) |
                          (uint32_t(data[i + 1]) << 8);
        out.push_back(kAlphabet[(triple >> 18) & 0x3F]);
        out.push_back(kAlphabet[(triple >> 12) & 0x3F]);
        out.push_back(kAlphabet[(triple >> 6) & 0x3F]);
    }
    return out;
}

// Minimal SHA-256.  Vendored to avoid pulling OpenSSL into this TU
// purely for PKCE.  Tested by RFC 6234 vectors in the test file.
struct Sha256 {
    uint32_t state[8];
    uint64_t bits;
    uint8_t buf[64];
    std::size_t buflen;

    static constexpr uint32_t K[64] = {
        0x428a2f98u,0x71374491u,0xb5c0fbcfu,0xe9b5dba5u,0x3956c25bu,0x59f111f1u,
        0x923f82a4u,0xab1c5ed5u,0xd807aa98u,0x12835b01u,0x243185beu,0x550c7dc3u,
        0x72be5d74u,0x80deb1feu,0x9bdc06a7u,0xc19bf174u,0xe49b69c1u,0xefbe4786u,
        0x0fc19dc6u,0x240ca1ccu,0x2de92c6fu,0x4a7484aau,0x5cb0a9dcu,0x76f988dau,
        0x983e5152u,0xa831c66du,0xb00327c8u,0xbf597fc7u,0xc6e00bf3u,0xd5a79147u,
        0x06ca6351u,0x14292967u,0x27b70a85u,0x2e1b2138u,0x4d2c6dfcu,0x53380d13u,
        0x650a7354u,0x766a0abbu,0x81c2c92eu,0x92722c85u,0xa2bfe8a1u,0xa81a664bu,
        0xc24b8b70u,0xc76c51a3u,0xd192e819u,0xd6990624u,0xf40e3585u,0x106aa070u,
        0x19a4c116u,0x1e376c08u,0x2748774cu,0x34b0bcb5u,0x391c0cb3u,0x4ed8aa4au,
        0x5b9cca4fu,0x682e6ff3u,0x748f82eeu,0x78a5636fu,0x84c87814u,0x8cc70208u,
        0x90befffau,0xa4506cebu,0xbef9a3f7u,0xc67178f2u};

    void reset() {
        state[0] = 0x6a09e667u; state[1] = 0xbb67ae85u;
        state[2] = 0x3c6ef372u; state[3] = 0xa54ff53au;
        state[4] = 0x510e527fu; state[5] = 0x9b05688cu;
        state[6] = 0x1f83d9abu; state[7] = 0x5be0cd19u;
        bits = 0; buflen = 0;
    }

    static uint32_t rotr(uint32_t x, unsigned n) {
        return (x >> n) | (x << (32 - n));
    }

    void transform(const uint8_t* data) {
        uint32_t w[64];
        for (int i = 0; i < 16; ++i) {
            w[i] = (uint32_t(data[i*4]) << 24) | (uint32_t(data[i*4+1]) << 16) |
                   (uint32_t(data[i*4+2]) << 8) | uint32_t(data[i*4+3]);
        }
        for (int i = 16; i < 64; ++i) {
            uint32_t s0 = rotr(w[i-15], 7) ^ rotr(w[i-15], 18) ^ (w[i-15] >> 3);
            uint32_t s1 = rotr(w[i-2], 17) ^ rotr(w[i-2], 19) ^ (w[i-2] >> 10);
            w[i] = w[i-16] + s0 + w[i-7] + s1;
        }
        uint32_t a = state[0], b = state[1], c = state[2], d = state[3];
        uint32_t e = state[4], f = state[5], g = state[6], h = state[7];
        for (int i = 0; i < 64; ++i) {
            uint32_t S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
            uint32_t ch = (e & f) ^ (~e & g);
            uint32_t t1 = h + S1 + ch + K[i] + w[i];
            uint32_t S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
            uint32_t mj = (a & b) ^ (a & c) ^ (b & c);
            uint32_t t2 = S0 + mj;
            h = g; g = f; f = e; e = d + t1;
            d = c; c = b; b = a; a = t1 + t2;
        }
        state[0] += a; state[1] += b; state[2] += c; state[3] += d;
        state[4] += e; state[5] += f; state[6] += g; state[7] += h;
    }

    void update(const uint8_t* data, std::size_t len) {
        bits += len * 8;
        while (len > 0) {
            const std::size_t take = std::min(len, std::size_t(64) - buflen);
            std::memcpy(buf + buflen, data, take);
            buflen += take; data += take; len -= take;
            if (buflen == 64) { transform(buf); buflen = 0; }
        }
    }

    void finish(uint8_t out[32]) {
        uint8_t pad[64] = {0};
        pad[0] = 0x80;
        const std::size_t padlen = (buflen < 56) ? (56 - buflen) : (120 - buflen);
        update(pad, padlen);
        uint8_t lenbuf[8];
        for (int i = 0; i < 8; ++i) {
            lenbuf[7 - i] = uint8_t((bits >> (i * 8)) & 0xFF);
        }
        bits -= 8 * 8;   // update() re-adds; compensate
        update(lenbuf, 8);
        for (int i = 0; i < 8; ++i) {
            out[i*4]     = uint8_t((state[i] >> 24) & 0xFF);
            out[i*4 + 1] = uint8_t((state[i] >> 16) & 0xFF);
            out[i*4 + 2] = uint8_t((state[i] >> 8) & 0xFF);
            out[i*4 + 3] = uint8_t(state[i] & 0xFF);
        }
    }
};
constexpr uint32_t Sha256::K[64];

void sha256_bytes(const unsigned char* data, std::size_t len, uint8_t out[32]) {
    Sha256 ctx;
    ctx.reset();
    ctx.update(data, len);
    ctx.finish(out);
}

// Known-model max-output table.  Longest-prefix substring wins.
struct OutLimit {
    const char* key;
    int limit;
};
constexpr std::array<OutLimit, 13> kOutLimits = {{
    {"claude-opus-4-6",   128000},
    {"claude-sonnet-4-6",  64000},
    {"claude-opus-4-5",    64000},
    {"claude-sonnet-4-5",  64000},
    {"claude-haiku-4-5",   64000},
    {"claude-opus-4",      32000},
    {"claude-sonnet-4",    64000},
    {"claude-3-7-sonnet", 128000},
    {"claude-3-5-sonnet",   8192},
    {"claude-3-5-haiku",    8192},
    {"claude-3-opus",       4096},
    {"claude-3-sonnet",     4096},
    {"claude-3-haiku",      4096},
}};
constexpr int kDefaultOutLimit = 128000;

}  // namespace

// ── max output ─────────────────────────────────────────────────────────

int get_anthropic_max_output_tokens(std::string_view model) {
    std::string m = to_lower(model);
    for (char& c : m) if (c == '.') c = '-';
    std::size_t best_len = 0;
    int best_val = kDefaultOutLimit;
    for (const auto& ol : kOutLimits) {
        std::string_view k(ol.key);
        if (m.find(k) != std::string::npos && k.size() > best_len) {
            best_len = k.size();
            best_val = ol.limit;
        }
    }
    if (contains_ci(m, "minimax")) return 131072;
    return best_val;
}

// ── normalisation ──────────────────────────────────────────────────────

std::string normalize_anthropic_model_name(std::string_view model,
                                           bool preserve_dots) {
    std::string s(model);
    if (starts_with_ci(s, "anthropic/")) {
        s = s.substr(std::strlen("anthropic/"));
    }
    if (!preserve_dots) {
        for (char& c : s) if (c == '.') c = '-';
    }
    return s;
}

std::string sanitize_anthropic_tool_id(std::string_view tool_id) {
    if (tool_id.empty()) return "tool_0";
    std::string out;
    out.reserve(tool_id.size());
    for (char c : tool_id) {
        const unsigned char u = static_cast<unsigned char>(c);
        if (std::isalnum(u) || c == '_' || c == '-') {
            out.push_back(c);
        } else {
            out.push_back('_');
        }
    }
    if (out.empty()) return "tool_0";
    return out;
}

// ── tool conversion ────────────────────────────────────────────────────

json convert_openai_tools_to_anthropic(const json& tools) {
    json out = json::array();
    if (!tools.is_array()) return out;
    for (const auto& t : tools) {
        if (!t.is_object()) continue;
        const json& fn = t.value("function", json::object());
        json entry;
        entry["name"] = fn.value("name", std::string{});
        entry["description"] = fn.value("description", std::string{});
        if (fn.contains("parameters") && fn["parameters"].is_object()) {
            entry["input_schema"] = fn["parameters"];
        } else {
            entry["input_schema"] = {{"type", "object"},
                                     {"properties", json::object()}};
        }
        out.push_back(std::move(entry));
    }
    return out;
}

// ── image source ───────────────────────────────────────────────────────

json image_source_from_openai_url(std::string_view url_in) {
    std::string url(url_in);
    // strip whitespace
    while (!url.empty() && std::isspace(static_cast<unsigned char>(url.front())))
        url.erase(url.begin());
    while (!url.empty() && std::isspace(static_cast<unsigned char>(url.back())))
        url.pop_back();
    if (url.empty()) {
        return json{{"type", "url"}, {"url", ""}};
    }
    if (starts_with(url, "data:")) {
        // data:<mime>[;base64],<payload>
        auto comma = url.find(',');
        std::string header = (comma == std::string::npos)
            ? url : url.substr(0, comma);
        std::string data = (comma == std::string::npos)
            ? std::string{} : url.substr(comma + 1);
        std::string media_type = "image/jpeg";
        if (starts_with(header, "data:")) {
            std::string rest = header.substr(5);
            auto semi = rest.find(';');
            std::string mime = (semi == std::string::npos)
                ? rest : rest.substr(0, semi);
            if (starts_with(mime, "image/")) media_type = mime;
        }
        return json{{"type", "base64"},
                    {"media_type", media_type},
                    {"data", data}};
    }
    return json{{"type", "url"}, {"url", url}};
}

// ── content part conversion ────────────────────────────────────────────

json convert_openai_content_part(const json& part) {
    if (part.is_null()) return json::object();
    if (part.is_string()) {
        return json{{"type", "text"}, {"text", part.get<std::string>()}};
    }
    if (!part.is_object()) {
        return json{{"type", "text"}, {"text", part.dump()}};
    }
    const std::string ptype = part.value("type", std::string{});
    json block;
    if (ptype == "input_text") {
        block = json{{"type", "text"}, {"text", part.value("text", std::string{})}};
    } else if (ptype == "image_url" || ptype == "input_image") {
        std::string url;
        if (part.contains("image_url")) {
            if (part["image_url"].is_object()) {
                url = part["image_url"].value("url", std::string{});
            } else if (part["image_url"].is_string()) {
                url = part["image_url"].get<std::string>();
            }
        }
        block = json{{"type", "image"},
                     {"source", image_source_from_openai_url(url)}};
    } else {
        block = part;
    }
    if (part.contains("cache_control") && part["cache_control"].is_object() &&
        !block.contains("cache_control")) {
        block["cache_control"] = part["cache_control"];
    }
    return block;
}

static json convert_content_array(const json& content) {
    if (!content.is_array()) return content;
    json out = json::array();
    for (const auto& p : content) {
        json b = convert_openai_content_part(p);
        if (!b.empty()) out.push_back(std::move(b));
    }
    return out;
}

// ── helpers: extract preserved thinking ─────────────────────────────────

static json extract_preserved_thinking(const json& message) {
    json out = json::array();
    if (!message.is_object()) return out;
    if (!message.contains("reasoning_details")) return out;
    const json& rd = message["reasoning_details"];
    if (!rd.is_array()) return out;
    for (const auto& detail : rd) {
        if (!detail.is_object()) continue;
        const std::string t = detail.value("type", std::string{});
        if (t == "thinking" || t == "redacted_thinking") {
            out.push_back(detail);
        }
    }
    return out;
}

// ── message conversion ─────────────────────────────────────────────────

namespace {

// Check whether a json content value has at least one non-empty text block.
bool content_has_nonempty_text(const json& content) {
    if (content.is_string()) {
        const std::string& s = content.get_ref<const std::string&>();
        for (char c : s) if (!std::isspace(static_cast<unsigned char>(c))) return true;
        return false;
    }
    if (!content.is_array()) return false;
    for (const auto& b : content) {
        if (!b.is_object()) continue;
        if (b.value("type", std::string{}) == "text") {
            const std::string t = b.value("text", std::string{});
            for (char c : t) if (!std::isspace(static_cast<unsigned char>(c))) return true;
        }
    }
    return false;
}

std::string join_text_blocks(const json& content) {
    if (!content.is_array()) return {};
    std::string out;
    bool first = true;
    for (const auto& b : content) {
        if (!b.is_object()) continue;
        if (b.value("type", std::string{}) != "text") continue;
        if (!first) out += "\n";
        out += b.value("text", std::string{});
        first = false;
    }
    return out;
}

bool has_cache_control(const json& content_array) {
    if (!content_array.is_array()) return false;
    for (const auto& p : content_array) {
        if (p.is_object() && p.contains("cache_control")) return true;
    }
    return false;
}

}  // namespace

ConvertedMessages convert_openai_messages_to_anthropic(
    const json& messages_in,
    std::string_view base_url) {
    ConvertedMessages out;
    if (!messages_in.is_array()) return out;

    // Pass 1: extract system + convert per-role.
    for (const auto& m : messages_in) {
        if (!m.is_object()) continue;
        const std::string role = m.value("role", std::string{"user"});
        const json& content = m.contains("content") ? m["content"] : json{};

        if (role == "system") {
            if (content.is_array()) {
                if (has_cache_control(content)) {
                    json arr = json::array();
                    for (const auto& p : content) {
                        if (p.is_object()) arr.push_back(p);
                    }
                    out.system = arr;
                } else {
                    out.system = join_text_blocks(content);
                }
            } else if (content.is_string()) {
                out.system = content;
            } else if (!content.is_null()) {
                out.system = content.dump();
            }
            continue;
        }

        if (role == "assistant") {
            json blocks = extract_preserved_thinking(m);
            if (!content.is_null() && !(content.is_string() &&
                                        content.get_ref<const std::string&>().empty())) {
                if (content.is_array()) {
                    json converted = convert_content_array(content);
                    if (converted.is_array()) {
                        for (auto& b : converted) blocks.push_back(b);
                    }
                } else if (content.is_string()) {
                    blocks.push_back({{"type", "text"},
                                      {"text", content.get<std::string>()}});
                }
            }
            if (m.contains("tool_calls") && m["tool_calls"].is_array()) {
                for (const auto& tc : m["tool_calls"]) {
                    if (!tc.is_object()) continue;
                    const json& fn = tc.value("function", json::object());
                    json parsed_args = json::object();
                    const json& args = fn.contains("arguments") ? fn["arguments"] : json{};
                    if (args.is_string()) {
                        try {
                            parsed_args = json::parse(args.get<std::string>());
                        } catch (...) { parsed_args = json::object(); }
                    } else if (args.is_object() || args.is_array()) {
                        parsed_args = args;
                    }
                    json block = {
                        {"type", "tool_use"},
                        {"id", sanitize_anthropic_tool_id(tc.value("id", std::string{}))},
                        {"name", fn.value("name", std::string{})},
                        {"input", parsed_args},
                    };
                    blocks.push_back(std::move(block));
                }
            }
            if (blocks.empty()) {
                blocks.push_back({{"type", "text"}, {"text", "(empty)"}});
            }
            out.messages.push_back({{"role", "assistant"}, {"content", blocks}});
            continue;
        }

        if (role == "tool") {
            std::string result_content;
            if (content.is_string()) result_content = content.get<std::string>();
            else if (!content.is_null()) result_content = content.dump();
            if (result_content.empty()) result_content = "(no output)";
            json tool_result = {
                {"type", "tool_result"},
                {"tool_use_id", sanitize_anthropic_tool_id(
                    m.value("tool_call_id", std::string{}))},
                {"content", result_content},
            };
            if (m.contains("cache_control") && m["cache_control"].is_object()) {
                tool_result["cache_control"] = m["cache_control"];
            }
            // Merge into previous user tool_result list.
            if (!out.messages.empty()) {
                auto& last = out.messages.back();
                if (last["role"] == "user" && last["content"].is_array() &&
                    !last["content"].empty() &&
                    last["content"][0].is_object() &&
                    last["content"][0].value("type", std::string{}) == "tool_result") {
                    last["content"].push_back(std::move(tool_result));
                    continue;
                }
            }
            out.messages.push_back({{"role", "user"},
                                    {"content", json::array({tool_result})}});
            continue;
        }

        // user
        if (content.is_array()) {
            json converted = convert_content_array(content);
            if (!content_has_nonempty_text(converted)) {
                converted = json::array({json{{"type", "text"},
                                              {"text", "(empty message)"}}});
            }
            out.messages.push_back({{"role", "user"}, {"content", converted}});
        } else if (content.is_string() &&
                   content_has_nonempty_text(content)) {
            out.messages.push_back({{"role", "user"}, {"content", content}});
        } else {
            out.messages.push_back({{"role", "user"},
                                    {"content", "(empty message)"}});
        }
    }

    // Pass 2: collect tool_use ids and tool_result ids.
    std::unordered_set<std::string> tool_result_ids;
    std::unordered_set<std::string> tool_use_ids;
    for (const auto& mm : out.messages) {
        if (mm["role"] == "user" && mm["content"].is_array()) {
            for (const auto& b : mm["content"]) {
                if (b.is_object() &&
                    b.value("type", std::string{}) == "tool_result") {
                    tool_result_ids.insert(b.value("tool_use_id", std::string{}));
                }
            }
        } else if (mm["role"] == "assistant" && mm["content"].is_array()) {
            for (const auto& b : mm["content"]) {
                if (b.is_object() &&
                    b.value("type", std::string{}) == "tool_use") {
                    tool_use_ids.insert(b.value("id", std::string{}));
                }
            }
        }
    }

    // Strip orphaned tool_use on assistants, tool_result on users.
    for (auto& mm : out.messages) {
        if (mm["role"] == "assistant" && mm["content"].is_array()) {
            json filtered = json::array();
            for (const auto& b : mm["content"]) {
                if (b.is_object() &&
                    b.value("type", std::string{}) == "tool_use") {
                    const std::string id = b.value("id", std::string{});
                    if (tool_result_ids.find(id) == tool_result_ids.end()) continue;
                }
                filtered.push_back(b);
            }
            if (filtered.empty()) {
                filtered.push_back({{"type", "text"},
                                    {"text", "(tool call removed)"}});
            }
            mm["content"] = filtered;
        } else if (mm["role"] == "user" && mm["content"].is_array()) {
            json filtered = json::array();
            for (const auto& b : mm["content"]) {
                if (b.is_object() &&
                    b.value("type", std::string{}) == "tool_result") {
                    const std::string id = b.value("tool_use_id", std::string{});
                    if (tool_use_ids.find(id) == tool_use_ids.end()) continue;
                }
                filtered.push_back(b);
            }
            if (filtered.empty()) {
                filtered.push_back({{"type", "text"},
                                    {"text", "(tool result removed)"}});
            }
            mm["content"] = filtered;
        }
    }

    // Pass 3: merge consecutive same-role messages.
    json merged = json::array();
    for (const auto& mm : out.messages) {
        if (!merged.empty() && merged.back()["role"] == mm["role"]) {
            auto& prev = merged.back();
            const std::string role = mm["role"].get<std::string>();
            json prev_c = prev["content"];
            json curr_c = mm["content"];
            if (role == "assistant" && curr_c.is_array()) {
                json scrubbed = json::array();
                for (const auto& b : curr_c) {
                    if (b.is_object()) {
                        const std::string t = b.value("type", std::string{});
                        if (t == "thinking" || t == "redacted_thinking") continue;
                    }
                    scrubbed.push_back(b);
                }
                curr_c = scrubbed;
            }
            if (prev_c.is_string() && curr_c.is_string()) {
                prev["content"] = prev_c.get<std::string>() + "\n" + curr_c.get<std::string>();
            } else {
                if (prev_c.is_string()) {
                    prev_c = json::array({json{{"type", "text"},
                                               {"text", prev_c.get<std::string>()}}});
                }
                if (curr_c.is_string()) {
                    curr_c = json::array({json{{"type", "text"},
                                               {"text", curr_c.get<std::string>()}}});
                }
                json combined = prev_c;
                for (auto& b : curr_c) combined.push_back(b);
                prev["content"] = combined;
            }
        } else {
            merged.push_back(mm);
        }
    }
    out.messages = merged;

    // Pass 4: thinking block signature strategy.
    const bool is_third_party = is_third_party_anthropic_endpoint(base_url);
    // Locate last assistant index.
    long last_assistant_idx = -1;
    for (long i = static_cast<long>(out.messages.size()) - 1; i >= 0; --i) {
        if (out.messages[static_cast<std::size_t>(i)]["role"] == "assistant") {
            last_assistant_idx = i;
            break;
        }
    }
    for (long idx = 0; idx < static_cast<long>(out.messages.size()); ++idx) {
        auto& mm = out.messages[static_cast<std::size_t>(idx)];
        if (mm["role"] != "assistant" || !mm["content"].is_array()) continue;
        const bool strip_all = is_third_party || idx != last_assistant_idx;
        json new_content = json::array();
        for (auto& b : mm["content"]) {
            if (!b.is_object()) { new_content.push_back(b); continue; }
            const std::string t = b.value("type", std::string{});
            if (t != "thinking" && t != "redacted_thinking") {
                new_content.push_back(b);
                continue;
            }
            if (strip_all) continue;
            // Latest assistant on native endpoint.
            if (t == "redacted_thinking") {
                if (b.contains("data") && !b["data"].is_null()) {
                    new_content.push_back(b);
                }
            } else {
                if (b.contains("signature") && !b["signature"].is_null()) {
                    new_content.push_back(b);
                } else {
                    const std::string txt = b.value("thinking", std::string{});
                    if (!txt.empty()) {
                        new_content.push_back({{"type", "text"}, {"text", txt}});
                    }
                }
            }
        }
        if (new_content.empty()) {
            new_content.push_back({{"type", "text"},
                                   {"text", strip_all ? "(thinking elided)" : "(empty)"}});
        }
        // Drop cache_control on any remaining thinking blocks.
        for (auto& b : new_content) {
            if (b.is_object()) {
                const std::string t = b.value("type", std::string{});
                if ((t == "thinking" || t == "redacted_thinking") &&
                    b.contains("cache_control")) {
                    b.erase("cache_control");
                }
            }
        }
        mm["content"] = new_content;
    }

    return out;
}

// ── request builder ────────────────────────────────────────────────────

namespace {
constexpr const char* kClaudeCodeSystemPrefix =
    "You are Claude Code, Anthropic's official CLI for Claude.";
constexpr const char* kMcpPrefix = "mcp_";
constexpr const char* kFastModeBeta = "fast-mode-2026-02-01";
constexpr const char* kOauthBeta1 = "claude-code-20250219";
constexpr const char* kOauthBeta2 = "oauth-2025-04-20";
}  // namespace

json build_anthropic_kwargs(const AnthropicBuildOptions& opts) {
    auto converted = convert_openai_messages_to_anthropic(opts.messages, opts.base_url);
    json anthropic_tools = convert_openai_tools_to_anthropic(opts.tools);

    const std::string model = normalize_anthropic_model_name(
        opts.model, opts.preserve_dots);
    int effective_max = opts.max_tokens.value_or(
        get_anthropic_max_output_tokens(model));
    if (opts.context_length && effective_max > *opts.context_length) {
        effective_max = std::max(*opts.context_length - 1, 1);
    }

    json system = converted.system;
    if (opts.is_oauth) {
        json cc_block = {{"type", "text"}, {"text", kClaudeCodeSystemPrefix}};
        json new_system = json::array();
        new_system.push_back(cc_block);
        if (system.is_array()) {
            for (auto& s : system) new_system.push_back(s);
        } else if (system.is_string() && !system.get<std::string>().empty()) {
            new_system.push_back({{"type", "text"}, {"text", system.get<std::string>()}});
        }
        // sanitize
        for (auto& block : new_system) {
            if (block.is_object() && block.value("type", std::string{}) == "text") {
                std::string t = block.value("text", std::string{});
                t = replace_all(t, "Hermes Agent", "Claude Code");
                t = replace_all(t, "Hermes agent", "Claude Code");
                t = replace_all(t, "hermes-agent", "claude-code");
                t = replace_all(t, "Nous Research", "Anthropic");
                block["text"] = t;
            }
        }
        system = new_system;
        // Prefix tool names with mcp_
        if (anthropic_tools.is_array()) {
            for (auto& tool : anthropic_tools) {
                if (tool.is_object() && tool.contains("name")) {
                    const std::string n = tool["name"].get<std::string>();
                    tool["name"] = apply_mcp_tool_prefix(n);
                }
            }
        }
        // Prefix tool_use names in message history.
        for (auto& mm : converted.messages) {
            if (mm["role"] == "assistant" && mm["content"].is_array()) {
                for (auto& b : mm["content"]) {
                    if (b.is_object() &&
                        b.value("type", std::string{}) == "tool_use" &&
                        b.contains("name")) {
                        const std::string n = b["name"].get<std::string>();
                        b["name"] = apply_mcp_tool_prefix(n);
                    }
                }
            }
        }
    }

    json kwargs;
    kwargs["model"] = model;
    kwargs["messages"] = converted.messages;
    kwargs["max_tokens"] = effective_max;

    if (!system.is_null()) {
        if (system.is_string()) {
            if (!system.get<std::string>().empty()) {
                kwargs["system"] = system;
            }
        } else {
            kwargs["system"] = system;
        }
    }

    if (!anthropic_tools.is_array() || anthropic_tools.empty()) {
        // no tools
    } else {
        kwargs["tools"] = anthropic_tools;
        const std::string tc = opts.tool_choice.value_or("auto");
        if (tc == "auto") {
            kwargs["tool_choice"] = {{"type", "auto"}};
        } else if (tc == "required" || tc == "any") {
            kwargs["tool_choice"] = {{"type", "any"}};
        } else if (tc == "none") {
            kwargs.erase("tools");
        } else {
            kwargs["tool_choice"] = {{"type", "tool"}, {"name", tc}};
        }
    }

    // Thinking config.
    if (opts.reasoning_config.is_object()) {
        const auto& rc = opts.reasoning_config;
        const bool enabled = rc.value("enabled", true);
        const std::string model_lower = to_lower(model);
        if (enabled && model_lower.find("haiku") == std::string::npos) {
            const std::string effort = to_lower(rc.value("effort", std::string{"medium"}));
            const int budget = thinking_budget_for_effort(effort);
            if (supports_adaptive_thinking(model)) {
                kwargs["thinking"] = {{"type", "adaptive"}};
                kwargs["output_config"] = {{"effort",
                    std::string(map_adaptive_effort(effort))}};
            } else {
                kwargs["thinking"] = {{"type", "enabled"},
                                      {"budget_tokens", budget}};
                kwargs["temperature"] = 1;
                kwargs["max_tokens"] = std::max(effective_max, budget + 4096);
            }
        }
    }

    // Fast mode (native only).
    if (opts.fast_mode && !is_third_party_anthropic_endpoint(opts.base_url)) {
        kwargs["speed"] = "fast";
        std::vector<std::string> betas = common_betas_for_base_url(opts.base_url);
        if (opts.is_oauth) {
            betas.emplace_back(kOauthBeta1);
            betas.emplace_back(kOauthBeta2);
        }
        betas.emplace_back(kFastModeBeta);
        std::string joined;
        for (std::size_t i = 0; i < betas.size(); ++i) {
            if (i) joined += ",";
            joined += betas[i];
        }
        kwargs["extra_headers"] = {{"anthropic-beta", joined}};
    }

    return kwargs;
}

// ── response normalisation ─────────────────────────────────────────────

NormalizedAnthropicResponse normalize_anthropic_response(
    const json& response, bool strip_tool_prefix) {
    NormalizedAnthropicResponse out;
    std::vector<std::string> text_parts;
    std::vector<std::string> reasoning_parts;
    json reasoning_details = json::array();
    json tool_calls = json::array();

    if (response.is_object() && response.contains("content") &&
        response["content"].is_array()) {
        for (const auto& block : response["content"]) {
            if (!block.is_object()) continue;
            const std::string type = block.value("type", std::string{});
            if (type == "text") {
                text_parts.push_back(block.value("text", std::string{}));
            } else if (type == "thinking") {
                reasoning_parts.push_back(block.value("thinking", std::string{}));
                reasoning_details.push_back(block);
            } else if (type == "redacted_thinking") {
                reasoning_details.push_back(block);
            } else if (type == "tool_use") {
                std::string name = block.value("name", std::string{});
                if (strip_tool_prefix && starts_with(name, "mcp_")) {
                    name = name.substr(4);
                }
                json call;
                call["id"] = block.value("id", std::string{});
                call["type"] = "function";
                json function_obj;
                function_obj["name"] = name;
                if (block.contains("input") && !block["input"].is_null()) {
                    function_obj["arguments"] = block["input"].dump();
                } else {
                    function_obj["arguments"] = "{}";
                }
                call["function"] = function_obj;
                tool_calls.push_back(std::move(call));
            }
        }
    }

    if (!text_parts.empty()) {
        std::string joined;
        for (std::size_t i = 0; i < text_parts.size(); ++i) {
            if (i) joined += "\n";
            joined += text_parts[i];
        }
        out.content = joined;
    }
    if (!tool_calls.empty()) out.tool_calls = tool_calls;
    if (!reasoning_parts.empty()) {
        std::string joined;
        for (std::size_t i = 0; i < reasoning_parts.size(); ++i) {
            if (i) joined += "\n\n";
            joined += reasoning_parts[i];
        }
        out.reasoning = joined;
    }
    if (!reasoning_details.empty()) out.reasoning_details = reasoning_details;

    const std::string stop = response.is_object()
        ? response.value("stop_reason", std::string{})
        : std::string{};
    out.finish_reason = map_anthropic_stop_reason(stop);

    if (response.is_object() && response.contains("usage")) {
        out.usage = response["usage"];
    }
    return out;
}

// ── OAuth helpers ───────────────────────────────────────────────────────

bool is_anthropic_oauth_token(std::string_view key) {
    if (key.empty()) return false;
    if (starts_with(key, "sk-ant-api")) return false;
    if (starts_with(key, "sk-ant-")) return true;
    if (starts_with(key, "eyJ")) return true;
    return false;
}

bool requires_bearer_auth(std::string_view base_url, std::string_view token) {
    if (!is_anthropic_oauth_token(token)) return false;
    if (is_third_party_anthropic_endpoint(base_url)) return false;
    return true;
}

static std::string url_encode(std::string_view s) {
    std::string out;
    out.reserve(s.size() * 3);
    for (unsigned char c : s) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            out.push_back(static_cast<char>(c));
        } else {
            static const char* hex = "0123456789ABCDEF";
            out.push_back('%');
            out.push_back(hex[c >> 4]);
            out.push_back(hex[c & 0xF]);
        }
    }
    return out;
}

std::string build_oauth_refresh_body(std::string_view refresh_token, bool use_json) {
    if (use_json) {
        json body = {
            {"grant_type", "refresh_token"},
            {"refresh_token", std::string(refresh_token)},
            {"client_id", "9d1c250a-e61b-44d9-88ed-5944d1962f5e"},
        };
        return body.dump();
    }
    std::string out;
    out += "grant_type=refresh_token";
    out += "&refresh_token=";
    out += url_encode(refresh_token);
    out += "&client_id=9d1c250a-e61b-44d9-88ed-5944d1962f5e";
    return out;
}

bool is_claude_code_token_valid(const json& creds) {
    if (!creds.is_object()) return false;
    if (!creds.contains("access_token") ||
        !creds["access_token"].is_string() ||
        creds["access_token"].get<std::string>().empty()) {
        return false;
    }
    // Expiry check — require at least 60s slack.
    if (creds.contains("expires_at") && creds["expires_at"].is_number()) {
        const int64_t exp_ms = creds["expires_at"].get<int64_t>();
        const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        if (exp_ms - now < 60000) return false;
    }
    return true;
}

PkcePair generate_pkce_pair(uint64_t rng_seed) {
    std::mt19937_64 rng(rng_seed != 0
        ? rng_seed
        : static_cast<uint64_t>(std::chrono::steady_clock::now()
                                    .time_since_epoch().count()));
    // 32 random bytes → base64url → ~43 chars.
    unsigned char buf[32];
    for (std::size_t i = 0; i < sizeof(buf); ++i) {
        buf[i] = static_cast<unsigned char>(rng() & 0xFF);
    }
    PkcePair out;
    out.code_verifier = urlsafe_b64(buf, sizeof(buf));
    uint8_t digest[32];
    sha256_bytes(reinterpret_cast<const unsigned char*>(out.code_verifier.data()),
                 out.code_verifier.size(), digest);
    out.code_challenge = urlsafe_b64(digest, sizeof(digest));
    out.method = "S256";
    return out;
}

// ── thinking strategy ──────────────────────────────────────────────────

ThinkingStrategy thinking_strategy_for_base_url(std::string_view base_url) {
    return is_third_party_anthropic_endpoint(base_url)
        ? ThinkingStrategy::StripAll
        : ThinkingStrategy::KeepLatestOnly;
}

// ── error classification ──────────────────────────────────────────────

AnthropicErrorKind classify_anthropic_error(int status, std::string_view body) {
    const std::string low = to_lower(body);
    if (status == 400) {
        if (contains_ci(low, "invalid signature") ||
            contains_ci(low, "signature in thinking")) {
            return AnthropicErrorKind::InvalidSignature;
        }
        if (contains_ci(low, "max_tokens")) {
            return AnthropicErrorKind::MaxTokensTooLarge;
        }
        if (contains_ci(low, "prompt is too long") ||
            contains_ci(low, "context length") ||
            contains_ci(low, "context window")) {
            return AnthropicErrorKind::ContextTooLong;
        }
        return AnthropicErrorKind::InvalidRequest;
    }
    if (status == 401) return AnthropicErrorKind::Authentication;
    if (status == 403) return AnthropicErrorKind::PermissionDenied;
    if (status == 404) return AnthropicErrorKind::NotFound;
    if (status == 413) return AnthropicErrorKind::RequestTooLarge;
    if (status == 429) return AnthropicErrorKind::RateLimit;
    if (status == 529) return AnthropicErrorKind::Overloaded;
    if (status == 500 || status == 502 || status == 503) {
        return AnthropicErrorKind::ServerError;
    }
    if (status == 504) return AnthropicErrorKind::GatewayTimeout;
    if (status == 408) return AnthropicErrorKind::Transient;
    return AnthropicErrorKind::Unknown;
}

bool anthropic_error_is_retryable(AnthropicErrorKind kind) {
    switch (kind) {
        case AnthropicErrorKind::Transient:
        case AnthropicErrorKind::RateLimit:
        case AnthropicErrorKind::Overloaded:
        case AnthropicErrorKind::ServerError:
        case AnthropicErrorKind::GatewayTimeout:
            return true;
        default:
            return false;
    }
}

std::optional<int> parse_available_max_tokens(std::string_view body) {
    // Pattern: "... > N, which is the maximum" or "maximum allowed is N".
    std::regex p1(R"(>\s*(\d{3,})\s*,?\s*(?:which\s+is\s+)?(?:the\s+)?maximum)",
                  std::regex::icase);
    std::regex p2(R"(max(?:imum)?\s+allowed\s+(?:is\s+)?(\d{3,}))",
                  std::regex::icase);
    std::regex p3(R"(max_tokens[^\d]*(\d{3,}))", std::regex::icase);
    std::cmatch m;
    const std::string s(body);
    if (std::regex_search(s.c_str(), m, p1)) {
        try { return std::stoi(m[1].str()); } catch (...) {}
    }
    if (std::regex_search(s.c_str(), m, p2)) {
        try { return std::stoi(m[1].str()); } catch (...) {}
    }
    if (std::regex_search(s.c_str(), m, p3)) {
        try { return std::stoi(m[1].str()); } catch (...) {}
    }
    return std::nullopt;
}

}  // namespace hermes::llm
