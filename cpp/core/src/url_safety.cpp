#include "hermes/core/url_safety.hpp"

#include "hermes/core/strings.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace hermes::core::url_safety {

namespace {

// Minimal URL parser — returns just the host portion, lowercased.
// Accepts `scheme://[user[:pass]@]host[:port][/path][?query][#frag]`.
std::string extract_host(std::string_view url) {
    const auto scheme_end = url.find("://");
    if (scheme_end == std::string_view::npos) {
        return {};
    }
    auto rest = url.substr(scheme_end + 3);
    // Strip user-info
    if (const auto at = rest.find('@'); at != std::string_view::npos) {
        rest = rest.substr(at + 1);
    }
    // Trim at first `/`, `?`, `#`.
    std::size_t cut = rest.size();
    for (char delim : {'/', '?', '#'}) {
        const auto p = rest.find(delim);
        if (p != std::string_view::npos && p < cut) {
            cut = p;
        }
    }
    auto host_port = rest.substr(0, cut);
    // Support [IPv6]:port — pull the bracketed portion as the host.
    if (!host_port.empty() && host_port.front() == '[') {
        const auto close = host_port.find(']');
        if (close == std::string_view::npos) {
            return {};
        }
        return hermes::core::strings::to_lower(host_port.substr(1, close - 1));
    }
    // Strip `:port`.
    if (const auto colon = host_port.find(':'); colon != std::string_view::npos) {
        host_port = host_port.substr(0, colon);
    }
    return hermes::core::strings::to_lower(host_port);
}

std::optional<std::array<std::uint8_t, 4>> parse_ipv4(std::string_view s) {
    std::array<std::uint8_t, 4> out{};
    std::size_t idx = 0;
    std::size_t pos = 0;
    while (idx < 4) {
        if (pos >= s.size()) {
            return std::nullopt;
        }
        unsigned value = 0;
        std::size_t digits = 0;
        while (pos < s.size() && s[pos] >= '0' && s[pos] <= '9') {
            value = value * 10 + static_cast<unsigned>(s[pos] - '0');
            if (value > 255 || ++digits > 3) {
                return std::nullopt;
            }
            ++pos;
        }
        if (digits == 0) {
            return std::nullopt;
        }
        out[idx++] = static_cast<std::uint8_t>(value);
        if (idx < 4) {
            if (pos >= s.size() || s[pos] != '.') {
                return std::nullopt;
            }
            ++pos;
        }
    }
    if (pos != s.size()) {
        return std::nullopt;
    }
    return out;
}

bool ipv4_in_private_range(const std::array<std::uint8_t, 4>& ip) {
    // 10.0.0.0/8
    if (ip[0] == 10) { return true; }
    // 127.0.0.0/8
    if (ip[0] == 127) { return true; }
    // 172.16.0.0/12
    if (ip[0] == 172 && (ip[1] & 0xF0) == 16) { return true; }
    // 192.168.0.0/16
    if (ip[0] == 192 && ip[1] == 168) { return true; }
    // 169.254.0.0/16 (link-local, includes cloud metadata)
    if (ip[0] == 169 && ip[1] == 254) { return true; }
    return false;
}

bool is_blocked_ipv6(std::string_view host) {
    const auto lower = hermes::core::strings::to_lower(host);
    if (lower == "::1" || lower == "::" || lower == "0:0:0:0:0:0:0:1") {
        return true;
    }
    // fe80::/10 (link-local)
    if (hermes::core::strings::starts_with(lower, "fe8") ||
        hermes::core::strings::starts_with(lower, "fe9") ||
        hermes::core::strings::starts_with(lower, "fea") ||
        hermes::core::strings::starts_with(lower, "feb")) {
        return true;
    }
    // fc00::/7 (unique local) -> fc** and fd**
    if (hermes::core::strings::starts_with(lower, "fc") ||
        hermes::core::strings::starts_with(lower, "fd")) {
        return true;
    }
    return false;
}

}  // namespace

bool is_private_address(std::string_view host_or_ip) {
    if (host_or_ip.empty()) {
        return true;
    }
    const auto host = hermes::core::strings::to_lower(host_or_ip);

    // Explicit metadata / loopback hostnames.
    if (host == "localhost" || host == "ip6-localhost" ||
        host == "metadata.google.internal" ||
        host == "metadata" ||
        host == "169.254.169.254") {
        return true;
    }
    if (hermes::core::strings::ends_with(host, ".localhost")) {
        return true;
    }

    if (const auto ipv4 = parse_ipv4(host)) {
        return ipv4_in_private_range(*ipv4);
    }
    if (host.find(':') != std::string::npos) {
        return is_blocked_ipv6(host);
    }
    return false;
}

bool is_safe_url(std::string_view url) {
    const auto host = extract_host(url);
    if (host.empty()) {
        return false;
    }
    return !is_private_address(host);
}

}  // namespace hermes::core::url_safety
