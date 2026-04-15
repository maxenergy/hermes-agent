// Depth port of agent/credential_pool.py helpers.  See the header for
// the Python-to-C++ mapping.

#include "hermes/agent/credential_pool_depth.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <ctime>
#include <regex>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_set>

namespace hermes::agent::creds::depth {

namespace {

constexpr std::int64_t kExhaustedTtl429 = 60 * 60;       // 1 hour
constexpr std::int64_t kExhaustedTtlDefault = 60 * 60;   // 1 hour
constexpr double kEpochMsThreshold = 1e12;  // > 1e12 ⇒ milliseconds

std::string to_lower(std::string_view s) {
    std::string out(s);
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

std::string trim(std::string_view s) {
    std::size_t start = 0;
    while (start < s.size() && std::isspace(static_cast<unsigned char>(s[start]))) ++start;
    std::size_t end = s.size();
    while (end > start && std::isspace(static_cast<unsigned char>(s[end - 1]))) --end;
    return std::string(s.substr(start, end - start));
}

std::string rtrim_slash(std::string_view s) {
    std::string out(s);
    while (!out.empty() && out.back() == '/') out.pop_back();
    return out;
}

// Parse a numeric timestamp, interpreting values > 1e12 as milliseconds.
std::optional<double> interpret_numeric(double v) {
    if (!(v > 0.0)) return std::nullopt;  // also rejects NaN
    return v > kEpochMsThreshold ? v / 1000.0 : v;
}

// A forgiving ISO-8601 parser.  Accepts:
//   YYYY-MM-DDTHH:MM:SS[.ffffff][Z|+HH:MM|-HH:MM]
// and the Python idiom where "Z" maps to "+00:00".  On any failure
// returns std::nullopt, matching Python's behaviour.
std::optional<double> parse_iso8601(std::string_view raw_in) {
    std::string raw = std::string(raw_in);
    // "Z" → "+00:00" (datetime.fromisoformat special-case)
    if (!raw.empty() && (raw.back() == 'Z' || raw.back() == 'z')) {
        raw.pop_back();
        raw += "+00:00";
    }
    // Regex for tz suffix: ±HH:MM
    std::regex tz_re(R"(([+-])(\d{2}):(\d{2})$)");
    std::smatch m;
    int tz_offset_seconds = 0;
    bool has_tz = false;
    if (std::regex_search(raw, m, tz_re)) {
        int sign = (m[1].str() == "-") ? -1 : 1;
        int hh = std::stoi(m[2].str());
        int mm = std::stoi(m[3].str());
        tz_offset_seconds = sign * (hh * 3600 + mm * 60);
        has_tz = true;
        raw = raw.substr(0, m.position(0));
    }
    // Parse "YYYY-MM-DDTHH:MM:SS[.frac]"
    std::regex dt_re(R"(^(\d{4})-(\d{2})-(\d{2})[T ](\d{2}):(\d{2}):(\d{2})(?:\.(\d+))?$)");
    if (!std::regex_match(raw, m, dt_re)) {
        return std::nullopt;
    }
    std::tm tm{};
    tm.tm_year = std::stoi(m[1].str()) - 1900;
    tm.tm_mon = std::stoi(m[2].str()) - 1;
    tm.tm_mday = std::stoi(m[3].str());
    tm.tm_hour = std::stoi(m[4].str());
    tm.tm_min = std::stoi(m[5].str());
    tm.tm_sec = std::stoi(m[6].str());
    double frac = 0.0;
    if (m[7].matched) {
        // Interpret fractional seconds as "0.xxx"
        std::string f = "0." + m[7].str();
        frac = std::stod(f);
    }
    // If has_tz, interpret as UTC epoch minus tz_offset.  timegm is
    // UTC-based; fallback to the classic offset adjustment otherwise.
#if defined(_WIN32)
    std::time_t secs = _mkgmtime(&tm);
#else
    std::time_t secs = ::timegm(&tm);
#endif
    if (secs == static_cast<std::time_t>(-1)) return std::nullopt;
    double out = static_cast<double>(secs) + frac;
    if (has_tz) {
        out -= tz_offset_seconds;
    }
    return out;
}

}  // namespace

bool is_supported_strategy(std::string_view name) {
    static const std::unordered_set<std::string> kSupported{
        "fill_first", "round_robin", "random", "least_used",
    };
    return kSupported.count(to_lower(name)) != 0;
}

std::int64_t exhausted_ttl(int error_code) {
    return error_code == 429 ? kExhaustedTtl429 : kExhaustedTtlDefault;
}

std::optional<double> parse_absolute_timestamp_numeric(double value) {
    return interpret_numeric(value);
}

std::optional<double> parse_absolute_timestamp_string(std::string_view raw_in) {
    std::string raw = trim(raw_in);
    if (raw.empty()) return std::nullopt;
    // Try numeric first (Python: float(raw) branch).
    try {
        std::size_t idx = 0;
        double v = std::stod(raw, &idx);
        // Whole string must be numeric for the numeric branch; Python
        // falls through to datetime otherwise.
        if (idx == raw.size()) {
            return interpret_numeric(v);
        }
    } catch (...) {
        // fall through to ISO-8601
    }
    return parse_iso8601(raw);
}

std::optional<double> extract_retry_delay_seconds(std::string_view message_in) {
    if (message_in.empty()) return std::nullopt;
    std::string message(message_in);
    // quotaResetDelay[:\s"]+(\d+(?:\.\d+)?)(ms|s)
    std::regex delay_re(
        R"(quotaResetDelay[:\s"]+([\d]+(?:\.[\d]+)?)(ms|s))",
        std::regex::icase);
    std::smatch m;
    if (std::regex_search(message, m, delay_re)) {
        double v = std::stod(m[1].str());
        std::string unit = to_lower(m[2].str());
        return unit == "ms" ? v / 1000.0 : v;
    }
    // retry\s+(?:after\s+)?(\d+(?:\.\d+)?)\s*(?:sec|secs|seconds|s\b)
    std::regex sec_re(
        R"(retry\s+(?:after\s+)?([\d]+(?:\.[\d]+)?)\s*(sec|secs|seconds|s\b))",
        std::regex::icase);
    if (std::regex_search(message, m, sec_re)) {
        return std::stod(m[1].str());
    }
    return std::nullopt;
}

NormalizedError normalize_error_context(const RawErrorContext& raw,
                                        double now_sec) {
    NormalizedError out;
    std::string reason = trim(raw.reason);
    if (!reason.empty()) out.reason = reason;
    std::string message = trim(raw.message);
    if (!message.empty()) out.message = message;

    std::optional<double> parsed;
    if (raw.reset_at_num.has_value()) {
        parsed = interpret_numeric(*raw.reset_at_num);
    }
    if (!parsed.has_value()) {
        for (const auto* src : {&raw.reset_at_str, &raw.resets_at_str, &raw.retry_until_str}) {
            if (!src->empty()) {
                parsed = parse_absolute_timestamp_string(*src);
                if (parsed.has_value()) break;
            }
        }
    }
    if (!parsed.has_value() && !message.empty()) {
        auto delay = extract_retry_delay_seconds(message);
        if (delay.has_value()) {
            parsed = now_sec + *delay;
        }
    }
    if (parsed.has_value()) out.reset_at = *parsed;
    return out;
}

std::string normalize_custom_pool_name(std::string_view name) {
    std::string lower = to_lower(trim(name));
    std::replace(lower.begin(), lower.end(), ' ', '-');
    return lower;
}

std::optional<std::string> custom_provider_pool_key(
    std::string_view base_url,
    const std::vector<CustomProviderEntry>& providers) {
    if (base_url.empty()) return std::nullopt;
    std::string normalized_url = rtrim_slash(trim(base_url));
    if (normalized_url.empty()) return std::nullopt;
    for (const auto& p : providers) {
        std::string entry_url = rtrim_slash(trim(p.base_url));
        if (!entry_url.empty() && entry_url == normalized_url) {
            return std::string(kCustomPoolPrefix) + p.normalized_name;
        }
    }
    return std::nullopt;
}

std::vector<std::string> list_custom_pool_providers(
    const std::vector<std::pair<std::string, std::size_t>>& pool_entries) {
    std::vector<std::string> out;
    const std::string prefix(kCustomPoolPrefix);
    for (const auto& [key, count] : pool_entries) {
        if (count == 0) continue;
        if (key.size() < prefix.size()) continue;
        if (key.compare(0, prefix.size(), prefix) != 0) continue;
        out.push_back(key);
    }
    std::sort(out.begin(), out.end());
    return out;
}

bool is_manual_source(std::string_view source) {
    std::string normalized = to_lower(trim(source));
    if (normalized == "manual") return true;
    const std::string prefix = "manual:";
    return normalized.size() > prefix.size()
        && normalized.compare(0, prefix.size(), prefix) == 0;
}

int next_priority(const std::vector<int>& priorities) {
    if (priorities.empty()) return 0;
    int mx = priorities.front();
    for (int p : priorities) mx = std::max(mx, p);
    return mx + 1;
}

std::string label_from_claims(
    const std::unordered_map<std::string, std::string>& claims,
    const std::string& fallback) {
    for (const auto* key : {"email", "preferred_username", "upn"}) {
        auto it = claims.find(key);
        if (it == claims.end()) continue;
        std::string trimmed = trim(it->second);
        if (!trimmed.empty()) return trimmed;
    }
    return fallback;
}

std::optional<double> exhausted_until(const EntrySnapshot& entry) {
    if (entry.last_status != "exhausted") return std::nullopt;
    if (entry.last_error_reset_at.has_value()) {
        auto parsed = interpret_numeric(*entry.last_error_reset_at);
        if (parsed.has_value()) return *parsed;
    }
    if (entry.last_status_at.has_value() && *entry.last_status_at > 0.0) {
        int code = entry.last_error_code.value_or(0);
        return *entry.last_status_at + static_cast<double>(exhausted_ttl(code));
    }
    return std::nullopt;
}

}  // namespace hermes::agent::creds::depth
