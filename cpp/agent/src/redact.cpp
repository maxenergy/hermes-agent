// C++17 port of agent/redact.py.
//
// Uses std::regex (ECMAScript flavor). Patterns are compiled lazily on
// first call and cached thread-locally to avoid contention in heavy
// logging paths.
#include "hermes/agent/redact.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cstdlib>
#include <mutex>
#include <regex>
#include <string>
#include <vector>

namespace hermes::agent::redact {

namespace {

std::atomic<int> g_enabled_cache{-1};  // -1 unset, 0 false, 1 true

bool compute_enabled() {
    const char* raw = std::getenv("HERMES_REDACT_SECRETS");
    if (raw == nullptr || *raw == '\0') {
        return true;
    }
    std::string lower(raw);
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return lower != "0" && lower != "false" && lower != "no" && lower != "off";
}

// Known API key prefixes — each alternation branch mirrors a Python entry.
const std::string& prefix_alternation() {
    static const std::string alt = []() {
        const std::array<const char*, 33> pats = {
            "sk-[A-Za-z0-9_-]{10,}",
            "ghp_[A-Za-z0-9]{10,}",
            "github_pat_[A-Za-z0-9_]{10,}",
            "gho_[A-Za-z0-9]{10,}",
            "ghu_[A-Za-z0-9]{10,}",
            "ghs_[A-Za-z0-9]{10,}",
            "ghr_[A-Za-z0-9]{10,}",
            "xox[baprs]-[A-Za-z0-9-]{10,}",
            "AIza[A-Za-z0-9_-]{30,}",
            "pplx-[A-Za-z0-9]{10,}",
            "fal_[A-Za-z0-9_-]{10,}",
            "fc-[A-Za-z0-9]{10,}",
            "bb_live_[A-Za-z0-9_-]{10,}",
            "gAAAA[A-Za-z0-9_=-]{20,}",
            "AKIA[A-Z0-9]{16}",
            "sk_live_[A-Za-z0-9]{10,}",
            "sk_test_[A-Za-z0-9]{10,}",
            "rk_live_[A-Za-z0-9]{10,}",
            "SG\\.[A-Za-z0-9_-]{10,}",
            "hf_[A-Za-z0-9]{10,}",
            "r8_[A-Za-z0-9]{10,}",
            "npm_[A-Za-z0-9]{10,}",
            "pypi-[A-Za-z0-9_-]{10,}",
            "dop_v1_[A-Za-z0-9]{10,}",
            "doo_v1_[A-Za-z0-9]{10,}",
            "am_[A-Za-z0-9_-]{10,}",
            "sk_[A-Za-z0-9_]{10,}",
            "tvly-[A-Za-z0-9]{10,}",
            "exa_[A-Za-z0-9]{10,}",
            "gsk_[A-Za-z0-9]{10,}",
            "syt_[A-Za-z0-9]{10,}",
            "retaindb_[A-Za-z0-9]{10,}",
            "hsk-[A-Za-z0-9]{10,}",
        };
        std::string s;
        s.reserve(1024);
        s += "(?:";
        bool first = true;
        for (auto* p : pats) {
            if (!first) s += '|';
            first = false;
            s += p;
        }
        s += ')';
        return s;
    }();
    return alt;
}

struct Patterns {
    std::regex prefix;
    std::regex env_assign;
    std::regex json_field;
    std::regex auth_header;
    std::regex telegram;
    std::regex private_key;
    std::regex db_connstr;
    std::regex phone;
};

const Patterns& patterns() {
    static const Patterns p = []() {
        Patterns out;
        // ECMAScript flavor supports lookahead/lookbehind in C++17 std::regex?
        // Lookbehind is NOT supported; simulate with explicit boundary group.
        // We approximate (?<![A-Za-z0-9_-]) by requiring either start-of-string
        // or a non-token char immediately before via a capture group.
        out.prefix = std::regex(
            "(^|[^A-Za-z0-9_-])(" + prefix_alternation() + ")(?![A-Za-z0-9_-])");
        out.env_assign = std::regex(
            "([A-Z0-9_]{0,50}(?:API_?KEY|TOKEN|SECRET|PASSWORD|PASSWD|CREDENTIAL|AUTH)"
            "[A-Z0-9_]{0,50})\\s*=\\s*(['\"]?)(\\S+)\\2");
        out.json_field = std::regex(
            "(\"(?:api_?[Kk]ey|token|secret|password|access_token|refresh_token|"
            "auth_token|bearer|secret_value|raw_secret|secret_input|key_material)\")"
            "\\s*:\\s*\"([^\"]+)\"",
            std::regex::icase);
        out.auth_header = std::regex(
            "(Authorization:\\s*Bearer\\s+)(\\S+)",
            std::regex::icase);
        out.telegram = std::regex(
            "(bot)?(\\d{8,}):([-A-Za-z0-9_]{30,})");
        out.private_key = std::regex(
            "-----BEGIN[A-Z ]*PRIVATE KEY-----[\\s\\S]*?-----END[A-Z ]*PRIVATE KEY-----");
        out.db_connstr = std::regex(
            "((?:postgres(?:ql)?|mysql|mongodb(?:\\+srv)?|redis|amqp)://[^:]+:)([^@]+)(@)",
            std::regex::icase);
        out.phone = std::regex("(\\+[1-9]\\d{6,14})(?![A-Za-z0-9])");
        return out;
    }();
    return p;
}

// Regex replacement with a callback; std::regex_replace only supports format
// strings, so we do it manually.
template <typename Fn>
std::string regex_replace_fn(const std::string& input, const std::regex& re, Fn&& fn) {
    std::string out;
    out.reserve(input.size());
    auto begin = std::sregex_iterator(input.begin(), input.end(), re);
    auto end = std::sregex_iterator();
    std::size_t last = 0;
    for (auto it = begin; it != end; ++it) {
        const auto& m = *it;
        auto pos = static_cast<std::size_t>(m.position());
        auto len = static_cast<std::size_t>(m.length());
        out.append(input, last, pos - last);
        out.append(fn(m));
        last = pos + len;
    }
    out.append(input, last, input.size() - last);
    return out;
}

}  // namespace

bool is_enabled() {
    int cached = g_enabled_cache.load(std::memory_order_acquire);
    if (cached == -1) {
        int v = compute_enabled() ? 1 : 0;
        g_enabled_cache.store(v, std::memory_order_release);
        return v == 1;
    }
    return cached == 1;
}

void reset_enabled_cache_for_testing() {
    g_enabled_cache.store(-1, std::memory_order_release);
}

std::string mask_token(const std::string& token) {
    if (token.size() < 18) {
        return "***";
    }
    return token.substr(0, 6) + "..." + token.substr(token.size() - 4);
}

std::string redact_sensitive_text(const std::string& text) {
    if (text.empty() || !is_enabled()) {
        return text;
    }

    const auto& p = patterns();
    std::string out = text;

    // Prefix patterns: capture group 1 = boundary prefix, group 2 = token.
    out = regex_replace_fn(out, p.prefix, [](const std::smatch& m) {
        return m[1].str() + mask_token(m[2].str());
    });

    // ENV assignments: KEY=value.
    out = regex_replace_fn(out, p.env_assign, [](const std::smatch& m) {
        const std::string& name = m[1].str();
        const std::string& quote = m[2].str();
        const std::string& value = m[3].str();
        return name + "=" + quote + mask_token(value) + quote;
    });

    // JSON fields: "apiKey": "value".
    out = regex_replace_fn(out, p.json_field, [](const std::smatch& m) {
        return m[1].str() + ": \"" + mask_token(m[2].str()) + "\"";
    });

    // Authorization headers.
    out = regex_replace_fn(out, p.auth_header, [](const std::smatch& m) {
        return m[1].str() + mask_token(m[2].str());
    });

    // Telegram bot tokens.
    out = regex_replace_fn(out, p.telegram, [](const std::smatch& m) {
        std::string prefix = m[1].matched ? m[1].str() : std::string();
        std::string digits = m[2].str();
        return prefix + digits + ":***";
    });

    // Private key blocks.
    out = regex_replace_fn(out, p.private_key, [](const std::smatch&) {
        return std::string("[REDACTED PRIVATE KEY]");
    });

    // DB connection strings.
    out = regex_replace_fn(out, p.db_connstr, [](const std::smatch& m) {
        return m[1].str() + "***" + m[3].str();
    });

    // E.164 phone numbers.
    out = regex_replace_fn(out, p.phone, [](const std::smatch& m) {
        const std::string phone = m[1].str();
        if (phone.size() <= 8) {
            return phone.substr(0, 2) + "****" + phone.substr(phone.size() - 2);
        }
        return phone.substr(0, 4) + "****" + phone.substr(phone.size() - 4);
    });

    return out;
}

}  // namespace hermes::agent::redact
