// C++17 port of agent/rate_limit_tracker.py.
#include "hermes/agent/rate_limit_tracker.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <sstream>
#include <string>
#include <vector>

namespace hermes::agent {

namespace {

double wall_now() {
    using namespace std::chrono;
    return duration<double>(system_clock::now().time_since_epoch()).count();
}

std::string to_lower(const std::string& s) {
    std::string out(s.size(), '\0');
    std::transform(s.begin(), s.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

std::int64_t safe_int(const std::string& s, std::int64_t def = 0) {
    if (s.empty()) return def;
    try {
        // Python uses int(float(v)) — support "42.5" -> 42 too.
        return static_cast<std::int64_t>(std::stod(s));
    } catch (...) {
        return def;
    }
}

double safe_double(const std::string& s, double def = 0.0) {
    if (s.empty()) return def;
    try {
        return std::stod(s);
    } catch (...) {
        return def;
    }
}

std::string lookup(const std::unordered_map<std::string, std::string>& m,
                   const std::string& key) {
    auto it = m.find(key);
    return it == m.end() ? std::string() : it->second;
}

std::string title_case(std::string s) {
    bool new_word = true;
    for (auto& c : s) {
        unsigned char uc = static_cast<unsigned char>(c);
        if (std::isspace(uc) || uc == '-' || uc == '_') {
            new_word = true;
        } else if (new_word) {
            c = static_cast<char>(std::toupper(uc));
            new_word = false;
        } else {
            c = static_cast<char>(std::tolower(uc));
        }
    }
    return s;
}

}  // namespace

// ── RateLimitBucket ────────────────────────────────────────────────────

std::int64_t RateLimitBucket::used() const noexcept {
    return std::max<std::int64_t>(0, limit - remaining);
}

double RateLimitBucket::usage_pct() const noexcept {
    if (limit <= 0) return 0.0;
    return (static_cast<double>(used()) / static_cast<double>(limit)) * 100.0;
}

double RateLimitBucket::remaining_seconds_now(double now) const noexcept {
    const double t = now > 0.0 ? now : wall_now();
    const double elapsed = t - captured_at;
    return std::max(0.0, reset_seconds - elapsed);
}

// ── RateLimitState ─────────────────────────────────────────────────────

double RateLimitState::age_seconds(double now) const noexcept {
    if (!has_data()) return std::numeric_limits<double>::infinity();
    const double t = now > 0.0 ? now : wall_now();
    return t - captured_at;
}

// ── Parser ─────────────────────────────────────────────────────────────

std::optional<RateLimitState> parse_rate_limit_headers(
    const std::unordered_map<std::string, std::string>& headers,
    const std::string& provider) {
    std::unordered_map<std::string, std::string> lowered;
    lowered.reserve(headers.size());
    for (const auto& [k, v] : headers) {
        lowered.emplace(to_lower(k), v);
    }

    bool has_any = false;
    for (const auto& [k, _v] : lowered) {
        if (k.rfind("x-ratelimit-", 0) == 0) {
            has_any = true;
            break;
        }
    }
    if (!has_any) return std::nullopt;

    const double now = wall_now();

    auto make_bucket = [&](const std::string& resource,
                           const std::string& suffix) {
        const std::string tag = resource + suffix;
        RateLimitBucket b;
        b.limit = safe_int(lookup(lowered, "x-ratelimit-limit-" + tag));
        b.remaining = safe_int(lookup(lowered, "x-ratelimit-remaining-" + tag));
        b.reset_seconds = safe_double(lookup(lowered, "x-ratelimit-reset-" + tag));
        b.captured_at = now;
        return b;
    };

    RateLimitState s;
    s.requests_min = make_bucket("requests", "");
    s.requests_hour = make_bucket("requests", "-1h");
    s.tokens_min = make_bucket("tokens", "");
    s.tokens_hour = make_bucket("tokens", "-1h");
    s.captured_at = now;
    s.provider = provider;
    return s;
}

// ── Formatting helpers ────────────────────────────────────────────────

namespace detail {

std::string fmt_count(std::int64_t n) {
    std::ostringstream os;
    os.setf(std::ios::fixed);
    os.precision(1);
    if (n >= 1000000) {
        os << (static_cast<double>(n) / 1000000.0) << "M";
    } else if (n >= 1000) {
        os << (static_cast<double>(n) / 1000.0) << "K";
    } else {
        os.precision(0);
        os << n;
    }
    return os.str();
}

std::string fmt_seconds(double seconds) {
    long s = std::max<long>(0, static_cast<long>(seconds));
    std::ostringstream os;
    if (s < 60) {
        os << s << "s";
    } else if (s < 3600) {
        long m = s / 60;
        long sec = s % 60;
        if (sec) os << m << "m " << sec << "s";
        else os << m << "m";
    } else {
        long h = s / 3600;
        long m = (s % 3600) / 60;
        if (m) os << h << "h " << m << "m";
        else os << h << "h";
    }
    return os.str();
}

std::string bar(double pct, int width) {
    int filled = static_cast<int>(pct / 100.0 * width);
    filled = std::clamp(filled, 0, width);
    const int empty = width - filled;
    std::string out;
    out.reserve(static_cast<std::size_t>(width) * 3 + 2);
    out += '[';
    for (int i = 0; i < filled; ++i) out += "█";
    for (int i = 0; i < empty; ++i) out += "░";
    out += ']';
    return out;
}

}  // namespace detail

namespace {

std::string bucket_line(const std::string& label,
                        const RateLimitBucket& b,
                        double now,
                        int label_width = 14) {
    std::ostringstream os;
    os << "  " << label;
    for (int i = static_cast<int>(label.size()); i < label_width; ++i) os << ' ';

    if (b.limit <= 0) {
        os << "  (no data)";
        return os.str();
    }

    os.setf(std::ios::fixed);
    os.precision(1);
    os << " " << detail::bar(b.usage_pct()) << " " << b.usage_pct() << "%  "
       << detail::fmt_count(b.used()) << "/" << detail::fmt_count(b.limit)
       << " used  (" << detail::fmt_count(b.remaining) << " left, resets in "
       << detail::fmt_seconds(b.remaining_seconds_now(now)) << ")";
    return os.str();
}

}  // namespace

std::string format_rate_limit_display(const RateLimitState& state, double now) {
    if (!state.has_data()) {
        return "No rate limit data yet — make an API request first.";
    }
    const double t = now > 0.0 ? now : wall_now();
    const double age = t - state.captured_at;

    std::string freshness;
    if (age < 5.0) freshness = "just now";
    else if (age < 60.0) freshness = std::to_string(static_cast<int>(age)) + "s ago";
    else freshness = detail::fmt_seconds(age) + " ago";

    std::string provider_label = state.provider.empty()
                                     ? std::string("Provider")
                                     : title_case(state.provider);

    std::ostringstream os;
    os << provider_label << " Rate Limits (captured " << freshness << "):\n\n"
       << bucket_line("Requests/min", state.requests_min, t) << "\n"
       << bucket_line("Requests/hr", state.requests_hour, t) << "\n\n"
       << bucket_line("Tokens/min", state.tokens_min, t) << "\n"
       << bucket_line("Tokens/hr", state.tokens_hour, t);

    struct Row {
        const char* label;
        const RateLimitBucket* bucket;
    };
    const Row rows[] = {
        {"requests/min", &state.requests_min},
        {"requests/hr", &state.requests_hour},
        {"tokens/min", &state.tokens_min},
        {"tokens/hr", &state.tokens_hour},
    };
    bool first_warn = true;
    for (const auto& r : rows) {
        if (r.bucket->limit > 0 && r.bucket->usage_pct() >= 80.0) {
            if (first_warn) { os << "\n"; first_warn = false; }
            os << "\n  ⚠ " << r.label << " at "
               << static_cast<int>(r.bucket->usage_pct()) << "% — resets in "
               << detail::fmt_seconds(r.bucket->remaining_seconds_now(t));
        }
    }
    return os.str();
}

std::string format_rate_limit_compact(const RateLimitState& state, double now) {
    if (!state.has_data()) return "No rate limit data.";
    const double t = now > 0.0 ? now : wall_now();

    std::vector<std::string> parts;
    const auto& rm = state.requests_min;
    const auto& rh = state.requests_hour;
    const auto& tm = state.tokens_min;
    const auto& th = state.tokens_hour;

    if (rm.limit > 0) {
        parts.push_back("RPM: " + std::to_string(rm.remaining) + "/" +
                        std::to_string(rm.limit));
    }
    if (rh.limit > 0) {
        parts.push_back("RPH: " + detail::fmt_count(rh.remaining) + "/" +
                        detail::fmt_count(rh.limit) + " (resets " +
                        detail::fmt_seconds(rh.remaining_seconds_now(t)) + ")");
    }
    if (tm.limit > 0) {
        parts.push_back("TPM: " + detail::fmt_count(tm.remaining) + "/" +
                        detail::fmt_count(tm.limit));
    }
    if (th.limit > 0) {
        parts.push_back("TPH: " + detail::fmt_count(th.remaining) + "/" +
                        detail::fmt_count(th.limit) + " (resets " +
                        detail::fmt_seconds(th.remaining_seconds_now(t)) + ")");
    }

    std::string out;
    for (std::size_t i = 0; i < parts.size(); ++i) {
        if (i) out += " | ";
        out += parts[i];
    }
    return out;
}

}  // namespace hermes::agent
