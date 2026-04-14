// C++17 port of foundational auth primitives from hermes_cli/auth.py.
#include "hermes/cli/auth_core.hpp"

#include "hermes/core/logging.hpp"
#include "hermes/core/path.hpp"

#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <random>
#include <regex>
#include <sstream>
#include <system_error>
#include <thread>

namespace hermes::cli::auth_core {

namespace fs = std::filesystem;

namespace {

// ---------------------------------------------------------------------------
// String helpers.
// ---------------------------------------------------------------------------

std::string strip(const std::string& s) {
    auto is_space = [](unsigned char c) { return std::isspace(c); };
    auto b = s.begin();
    while (b != s.end() && is_space(*b)) ++b;
    auto e = s.end();
    while (e != b && is_space(*(e - 1))) --e;
    return std::string(b, e);
}

std::string to_lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    return s;
}

std::string rstrip_slash(std::string s) {
    while (!s.empty() && s.back() == '/') s.pop_back();
    return s;
}

std::string read_file(const fs::path& p) {
    std::ifstream ifs(p, std::ios::binary);
    if (!ifs) return {};
    std::ostringstream ss;
    ss << ifs.rdbuf();
    return ss.str();
}

bool write_file_atomic(const fs::path& p, const std::string& content,
                       mode_t mode = 0600) {
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    auto tmp = p;
    tmp += ".tmp";
    {
        std::ofstream ofs(tmp, std::ios::binary | std::ios::trunc);
        if (!ofs) return false;
        ofs.write(content.data(),
                  static_cast<std::streamsize>(content.size()));
        if (!ofs) return false;
    }
    ::chmod(tmp.c_str(), mode);
    fs::rename(tmp, p, ec);
    return !ec;
}

// ---------------------------------------------------------------------------
// SHA-256 — compact pure-C++ implementation.  Used only for 12-char
// fingerprints of OAuth tokens; never for secrets-in-motion.
// ---------------------------------------------------------------------------

class Sha256 {
public:
    Sha256() { reset(); }

    void reset() {
        data_len_ = 0;
        bit_len_ = 0;
        state_[0] = 0x6a09e667;
        state_[1] = 0xbb67ae85;
        state_[2] = 0x3c6ef372;
        state_[3] = 0xa54ff53a;
        state_[4] = 0x510e527f;
        state_[5] = 0x9b05688c;
        state_[6] = 0x1f83d9ab;
        state_[7] = 0x5be0cd19;
    }

    void update(const unsigned char* data, std::size_t len) {
        for (std::size_t i = 0; i < len; ++i) {
            buffer_[data_len_++] = data[i];
            if (data_len_ == 64) {
                transform(buffer_);
                bit_len_ += 512;
                data_len_ = 0;
            }
        }
    }

    std::array<unsigned char, 32> finalize() {
        std::size_t i = data_len_;
        if (data_len_ < 56) {
            buffer_[i++] = 0x80;
            while (i < 56) buffer_[i++] = 0;
        } else {
            buffer_[i++] = 0x80;
            while (i < 64) buffer_[i++] = 0;
            transform(buffer_);
            std::memset(buffer_, 0, 56);
        }
        bit_len_ += data_len_ * 8;
        for (int j = 0; j < 8; ++j) {
            buffer_[63 - j] = static_cast<unsigned char>(bit_len_ >> (8 * j));
        }
        transform(buffer_);
        std::array<unsigned char, 32> out{};
        for (int j = 0; j < 4; ++j) {
            for (int k = 0; k < 8; ++k) {
                out[j + k * 4] =
                    static_cast<unsigned char>((state_[k] >> (24 - j * 8)) & 0xFF);
            }
        }
        return out;
    }

private:
    static std::uint32_t rotr(std::uint32_t x, std::uint32_t n) {
        return (x >> n) | (x << (32 - n));
    }

    void transform(const unsigned char block[64]) {
        static const std::uint32_t K[64] = {
            0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b,
            0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01,
            0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7,
            0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
            0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152,
            0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
            0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc,
            0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
            0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819,
            0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08,
            0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f,
            0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
            0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
        std::uint32_t w[64];
        for (int i = 0; i < 16; ++i) {
            w[i] = (std::uint32_t(block[i * 4]) << 24) |
                   (std::uint32_t(block[i * 4 + 1]) << 16) |
                   (std::uint32_t(block[i * 4 + 2]) << 8) |
                   std::uint32_t(block[i * 4 + 3]);
        }
        for (int i = 16; i < 64; ++i) {
            std::uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^
                               (w[i - 15] >> 3);
            std::uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^
                               (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        std::uint32_t a = state_[0], b = state_[1], c = state_[2],
                      d = state_[3], e = state_[4], f = state_[5],
                      g = state_[6], h = state_[7];
        for (int i = 0; i < 64; ++i) {
            std::uint32_t S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
            std::uint32_t ch = (e & f) ^ (~e & g);
            std::uint32_t temp1 = h + S1 + ch + K[i] + w[i];
            std::uint32_t S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
            std::uint32_t mj = (a & b) ^ (a & c) ^ (b & c);
            std::uint32_t temp2 = S0 + mj;
            h = g;
            g = f;
            f = e;
            e = d + temp1;
            d = c;
            c = b;
            b = a;
            a = temp1 + temp2;
        }
        state_[0] += a;
        state_[1] += b;
        state_[2] += c;
        state_[3] += d;
        state_[4] += e;
        state_[5] += f;
        state_[6] += g;
        state_[7] += h;
    }

    unsigned char buffer_[64]{};
    std::size_t data_len_ = 0;
    std::uint64_t bit_len_ = 0;
    std::uint32_t state_[8]{};
};

std::string sha256_hex(const std::string& s) {
    Sha256 h;
    h.update(reinterpret_cast<const unsigned char*>(s.data()), s.size());
    auto digest = h.finalize();
    std::ostringstream out;
    out << std::hex << std::setfill('0');
    for (auto b : digest) out << std::setw(2) << static_cast<int>(b);
    return out.str();
}

// ---------------------------------------------------------------------------
// URL-safe base64 decoder (for JWT payloads).
// ---------------------------------------------------------------------------

std::optional<std::string> b64url_decode(std::string s) {
    // Convert URL-safe alphabet to standard.
    for (auto& c : s) {
        if (c == '-') c = '+';
        else if (c == '_') c = '/';
    }
    // Pad to multiple of 4.
    while (s.size() % 4) s.push_back('=');

    static const std::array<int, 256> table = []() {
        std::array<int, 256> t{};
        for (auto& v : t) v = -1;
        const char* alph =
            "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        for (int i = 0; i < 64; ++i) {
            t[static_cast<unsigned char>(alph[i])] = i;
        }
        t[static_cast<unsigned char>('=')] = -2;
        return t;
    }();

    std::string out;
    out.reserve(s.size() * 3 / 4);
    int buf = 0;
    int bits = 0;
    for (unsigned char c : s) {
        int v = table[c];
        if (v == -2) break;
        if (v == -1) {
            if (std::isspace(c)) continue;
            return std::nullopt;
        }
        buf = (buf << 6) | v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<char>((buf >> bits) & 0xFF));
        }
    }
    return out;
}

// ---------------------------------------------------------------------------
// Reentrant thread-local counter for the advisory lock.
// ---------------------------------------------------------------------------

thread_local int g_auth_lock_depth = 0;

// ---------------------------------------------------------------------------
// Getenv helpers.
// ---------------------------------------------------------------------------

std::string default_env_lookup(const std::string& key) {
    const char* v = std::getenv(key.c_str());
    return v ? std::string(v) : std::string();
}

}  // namespace

// ---------------------------------------------------------------------------
// Implicit env vars table.
// ---------------------------------------------------------------------------

const std::unordered_set<std::string>& implicit_env_vars() {
    static const std::unordered_set<std::string> s = {
        "CLAUDE_CODE_OAUTH_TOKEN",
    };
    return s;
}

// ---------------------------------------------------------------------------
// AuthError.
// ---------------------------------------------------------------------------

AuthError::AuthError(const std::string& message, const std::string& provider,
                     const std::optional<std::string>& code,
                     bool relogin_required)
    : std::runtime_error(message),
      provider_(provider),
      code_(code),
      relogin_required_(relogin_required) {}

std::string format_auth_error(const std::exception& error) {
    const auto* ae = dynamic_cast<const AuthError*>(&error);
    if (!ae) return error.what();

    std::string base = ae->what();
    if (ae->relogin_required()) {
        return base + " Run `hermes model` to re-authenticate.";
    }
    if (ae->code()) {
        const auto& c = *ae->code();
        if (c == "subscription_required") {
            return "No active paid subscription found on Nous Portal. "
                   "Please purchase/activate a subscription, then retry.";
        }
        if (c == "insufficient_credits") {
            return "Subscription credits are exhausted. "
                   "Top up/renew credits in Nous Portal, then retry.";
        }
        if (c == "temporarily_unavailable") {
            return base + " Please retry in a few seconds.";
        }
    }
    return base;
}

// ---------------------------------------------------------------------------
// Token / JWT helpers.
// ---------------------------------------------------------------------------

std::optional<std::string> token_fingerprint(const std::string& token) {
    auto cleaned = strip(token);
    if (cleaned.empty()) return std::nullopt;
    auto hex = sha256_hex(cleaned);
    return hex.substr(0, 12);
}

bool oauth_trace_enabled() {
    auto raw = to_lower(strip(default_env_lookup("HERMES_OAUTH_TRACE")));
    return raw == "1" || raw == "true" || raw == "yes" || raw == "on";
}

bool has_usable_secret(const std::string& value, std::size_t min_length) {
    auto cleaned = strip(value);
    return cleaned.size() >= min_length;
}

bool has_usable_secret(const nlohmann::json& value, std::size_t min_length) {
    if (!value.is_string()) return false;
    return has_usable_secret(value.get<std::string>(), min_length);
}

std::optional<double> parse_iso_timestamp(const std::string& value) {
    auto text = strip(value);
    if (text.empty()) return std::nullopt;

    // Rewrite trailing Z as +00:00 to match datetime.fromisoformat().
    if (!text.empty() && text.back() == 'Z') {
        text = text.substr(0, text.size() - 1) + "+00:00";
    }

    // Regex: YYYY-MM-DD[T ]HH:MM:SS[.fff][(+|-)HH:MM]
    static const std::regex kIso(
        R"(^(\d{4})-(\d{2})-(\d{2})[Tt ](\d{2}):(\d{2}):(\d{2})(?:\.(\d+))?)"
        R"((?:([+-])(\d{2}):(\d{2}))?$)");
    std::smatch m;
    if (!std::regex_match(text, m, kIso)) return std::nullopt;

    std::tm tm{};
    tm.tm_year = std::stoi(m[1]) - 1900;
    tm.tm_mon = std::stoi(m[2]) - 1;
    tm.tm_mday = std::stoi(m[3]);
    tm.tm_hour = std::stoi(m[4]);
    tm.tm_min = std::stoi(m[5]);
    tm.tm_sec = std::stoi(m[6]);

    // timegm-style computation (UTC).
    std::time_t t = ::timegm(&tm);
    if (t == static_cast<std::time_t>(-1)) return std::nullopt;

    double epoch = static_cast<double>(t);
    if (m[7].matched) {
        std::string frac = m[7].str();
        double denom = std::pow(10.0, static_cast<double>(frac.size()));
        epoch += std::stod(frac) / denom;
    }
    if (m[8].matched) {
        int sign = (m[8].str() == "+") ? 1 : -1;
        int tz_h = std::stoi(m[9]);
        int tz_m = std::stoi(m[10]);
        // Local text → UTC: subtract the offset.
        epoch -= sign * (tz_h * 3600 + tz_m * 60);
    }
    return epoch;
}

std::optional<double> parse_iso_timestamp(const nlohmann::json& value) {
    if (!value.is_string()) return std::nullopt;
    return parse_iso_timestamp(value.get<std::string>());
}

bool is_expiring(const nlohmann::json& expires_at_iso, int skew_seconds) {
    auto epoch = parse_iso_timestamp(expires_at_iso);
    if (!epoch) return true;
    double now = static_cast<double>(std::time(nullptr));
    return *epoch <= (now + skew_seconds);
}

int coerce_ttl_seconds(const nlohmann::json& expires_in) {
    int ttl = 0;
    if (expires_in.is_number_integer() || expires_in.is_number_unsigned()) {
        ttl = static_cast<int>(expires_in.get<long long>());
    } else if (expires_in.is_number_float()) {
        ttl = static_cast<int>(expires_in.get<double>());
    } else if (expires_in.is_string()) {
        try {
            ttl = std::stoi(expires_in.get<std::string>());
        } catch (...) {
            ttl = 0;
        }
    }
    return std::max(0, ttl);
}

std::optional<std::string> optional_base_url(const nlohmann::json& value) {
    if (!value.is_string()) return std::nullopt;
    auto cleaned = rstrip_slash(strip(value.get<std::string>()));
    if (cleaned.empty()) return std::nullopt;
    return cleaned;
}

nlohmann::json decode_jwt_claims(const std::string& token) {
    auto empty = nlohmann::json::object();
    std::size_t dot_count = std::count(token.begin(), token.end(), '.');
    if (dot_count != 2) return empty;
    auto first_dot = token.find('.');
    auto second_dot = token.find('.', first_dot + 1);
    std::string payload = token.substr(first_dot + 1,
                                       second_dot - first_dot - 1);
    auto decoded = b64url_decode(payload);
    if (!decoded) return empty;
    try {
        auto parsed = nlohmann::json::parse(*decoded);
        if (parsed.is_object()) return parsed;
    } catch (...) {}
    return empty;
}

bool codex_access_token_is_expiring(const std::string& access_token,
                                    int skew_seconds) {
    auto claims = decode_jwt_claims(access_token);
    auto it = claims.find("exp");
    if (it == claims.end()) return false;
    double exp = 0.0;
    if (it->is_number()) {
        exp = it->get<double>();
    } else {
        return false;
    }
    double now = static_cast<double>(std::time(nullptr));
    return exp <= (now + skew_seconds);
}

// ---------------------------------------------------------------------------
// Auth store paths + lock.
// ---------------------------------------------------------------------------

fs::path auth_file_path() {
    return hermes::core::path::get_hermes_home() / "auth.json";
}

fs::path auth_lock_path() {
    auto p = auth_file_path();
    return fs::path(p.string() + ".lock");
}

AuthStoreLock::AuthStoreLock(double timeout_seconds) {
    if (g_auth_lock_depth > 0) {
        ++g_auth_lock_depth;
        reentered_ = true;
        acquired_ = true;
        return;
    }
    auto lock_path = auth_lock_path();
    std::error_code ec;
    fs::create_directories(lock_path.parent_path(), ec);
    fd_ = ::open(lock_path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (fd_ < 0) return;

    auto deadline = std::chrono::steady_clock::now() +
                    std::chrono::milliseconds(
                        static_cast<long long>(timeout_seconds * 1000));
    while (true) {
        if (::flock(fd_, LOCK_EX | LOCK_NB) == 0) {
            acquired_ = true;
            ++g_auth_lock_depth;
            return;
        }
        if (errno != EWOULDBLOCK) return;
        if (std::chrono::steady_clock::now() >= deadline) return;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
}

AuthStoreLock::~AuthStoreLock() {
    if (reentered_) {
        --g_auth_lock_depth;
        return;
    }
    if (acquired_) {
        ::flock(fd_, LOCK_UN);
        --g_auth_lock_depth;
    }
    if (fd_ >= 0) ::close(fd_);
}

// ---------------------------------------------------------------------------
// Auth store JSON I/O.
// ---------------------------------------------------------------------------

nlohmann::json load_auth_store(const fs::path& path) {
    std::error_code ec;
    if (!fs::exists(path, ec)) return nlohmann::json::object();
    auto text = read_file(path);
    if (text.empty()) return nlohmann::json::object();
    try {
        auto parsed = nlohmann::json::parse(text);
        if (parsed.is_object()) return parsed;
    } catch (const std::exception& e) {
        hermes::core::logging::log_debug(
            std::string("auth_core.load_auth_store: ") + e.what());
    }
    return nlohmann::json::object();
}

nlohmann::json load_auth_store() {
    return load_auth_store(auth_file_path());
}

fs::path save_auth_store(const nlohmann::json& auth_store,
                         const fs::path& path) {
    // Pretty-print with 2-space indent for easier human inspection.
    write_file_atomic(path, auth_store.dump(2), 0600);
    return path;
}

fs::path save_auth_store(const nlohmann::json& auth_store) {
    return save_auth_store(auth_store, auth_file_path());
}

// ---------------------------------------------------------------------------
// Per-provider slot accessors.
// ---------------------------------------------------------------------------

std::optional<nlohmann::json> load_provider_state(
    const nlohmann::json& auth_store, const std::string& provider_id) {
    if (!auth_store.is_object()) return std::nullopt;
    auto providers_it = auth_store.find("providers");
    if (providers_it == auth_store.end() || !providers_it->is_object()) {
        return std::nullopt;
    }
    auto p_it = providers_it->find(provider_id);
    if (p_it == providers_it->end()) return std::nullopt;
    return *p_it;
}

void save_provider_state(nlohmann::json& auth_store,
                         const std::string& provider_id,
                         const nlohmann::json& state) {
    if (!auth_store.contains("providers") ||
        !auth_store["providers"].is_object()) {
        auth_store["providers"] = nlohmann::json::object();
    }
    auth_store["providers"][provider_id] = state;
}

// ---------------------------------------------------------------------------
// Active-provider tracking.
// ---------------------------------------------------------------------------

std::optional<std::string> get_active_provider() {
    auto store = load_auth_store();
    auto it = store.find("active_provider");
    if (it == store.end() || !it->is_string()) return std::nullopt;
    auto s = it->get<std::string>();
    if (s.empty()) return std::nullopt;
    return s;
}

std::optional<nlohmann::json> get_provider_auth_state(
    const std::string& provider_id) {
    auto store = load_auth_store();
    return load_provider_state(store, provider_id);
}

bool clear_provider_auth(const std::string& provider_id) {
    AuthStoreLock lock;
    auto store = load_auth_store();
    bool changed = false;
    if (provider_id.empty()) {
        if (store.contains("providers") &&
            !store["providers"].empty()) {
            store["providers"] = nlohmann::json::object();
            changed = true;
        }
        if (store.contains("active_provider")) {
            store.erase("active_provider");
            changed = true;
        }
    } else {
        auto p_it = store.find("providers");
        if (p_it != store.end() && p_it->is_object() &&
            p_it->erase(provider_id) > 0) {
            changed = true;
        }
        if (store.value("active_provider", "") == provider_id) {
            store.erase("active_provider");
            changed = true;
        }
    }
    if (changed) save_auth_store(store);
    return changed;
}

void deactivate_provider() {
    AuthStoreLock lock;
    auto store = load_auth_store();
    if (store.contains("active_provider")) {
        store.erase("active_provider");
        save_auth_store(store);
    }
}

// ---------------------------------------------------------------------------
// Credential pool helpers.
// ---------------------------------------------------------------------------

nlohmann::json read_credential_pool() {
    auto store = load_auth_store();
    auto it = store.find("credential_pool");
    if (it == store.end() || !it->is_object()) {
        return nlohmann::json::object();
    }
    return *it;
}

nlohmann::json read_credential_pool(const std::string& provider_id) {
    auto store = load_auth_store();
    auto it = store.find("credential_pool");
    if (it == store.end() || !it->is_object()) {
        return nlohmann::json::array();
    }
    auto p_it = it->find(provider_id);
    if (p_it == it->end() || !p_it->is_array()) {
        return nlohmann::json::array();
    }
    return *p_it;
}

fs::path write_credential_pool(const std::string& provider_id,
                               const std::vector<nlohmann::json>& entries) {
    AuthStoreLock lock;
    auto store = load_auth_store();
    if (!store.contains("credential_pool") ||
        !store["credential_pool"].is_object()) {
        store["credential_pool"] = nlohmann::json::object();
    }
    auto arr = nlohmann::json::array();
    for (const auto& e : entries) arr.push_back(e);
    store["credential_pool"][provider_id] = arr;
    return save_auth_store(store);
}

void suppress_credential_source(const std::string& provider_id,
                                const std::string& source) {
    AuthStoreLock lock;
    auto store = load_auth_store();
    if (!store.contains("suppressed_sources") ||
        !store["suppressed_sources"].is_object()) {
        store["suppressed_sources"] = nlohmann::json::object();
    }
    auto& prov = store["suppressed_sources"][provider_id];
    if (!prov.is_array()) prov = nlohmann::json::array();
    bool exists = false;
    for (const auto& v : prov) {
        if (v.is_string() && v.get<std::string>() == source) {
            exists = true;
            break;
        }
    }
    if (!exists) prov.push_back(source);
    save_auth_store(store);
}

bool is_source_suppressed(const std::string& provider_id,
                          const std::string& source) {
    try {
        auto store = load_auth_store();
        auto s_it = store.find("suppressed_sources");
        if (s_it == store.end() || !s_it->is_object()) return false;
        auto p_it = s_it->find(provider_id);
        if (p_it == s_it->end() || !p_it->is_array()) return false;
        for (const auto& v : *p_it) {
            if (v.is_string() && v.get<std::string>() == source) return true;
        }
    } catch (...) {}
    return false;
}

// ---------------------------------------------------------------------------
// Provider registry.
// ---------------------------------------------------------------------------

const std::unordered_map<std::string, ProviderConfig>& provider_registry() {
    static const auto build = []() {
        std::unordered_map<std::string, ProviderConfig> m;
        auto add = [&](const ProviderConfig& pc) { m.emplace(pc.id, pc); };

        add({"anthropic",
             "Anthropic (Claude)",
             kAuthTypeApiKey,
             "https://api.anthropic.com",
             {"ANTHROPIC_API_KEY"},
             "https://console.anthropic.com/",
             false,
             true});
        add({"openai",
             "OpenAI",
             kAuthTypeApiKey,
             "https://api.openai.com/v1",
             {"OPENAI_API_KEY"},
             "https://platform.openai.com/api-keys",
             false,
             true});
        add({"openrouter",
             "OpenRouter",
             kAuthTypeApiKey,
             "https://openrouter.ai/api/v1",
             {"OPENROUTER_API_KEY"},
             "https://openrouter.ai/keys",
             false,
             true});
        add({"google",
             "Google Gemini",
             kAuthTypeApiKey,
             "https://generativelanguage.googleapis.com/v1beta/openai",
             {"GEMINI_API_KEY", "GOOGLE_API_KEY"},
             "https://aistudio.google.com/apikey",
             false,
             true});
        add({"gemini",
             "Google Gemini",
             kAuthTypeApiKey,
             "https://generativelanguage.googleapis.com/v1beta/openai",
             {"GEMINI_API_KEY", "GOOGLE_API_KEY"},
             "https://aistudio.google.com/apikey",
             false,
             true});
        add({"deepseek",
             "DeepSeek",
             kAuthTypeApiKey,
             "https://api.deepseek.com",
             {"DEEPSEEK_API_KEY"},
             "https://platform.deepseek.com/api_keys",
             false,
             true});
        add({"mistral",
             "Mistral",
             kAuthTypeApiKey,
             "https://api.mistral.ai/v1",
             {"MISTRAL_API_KEY"},
             "https://console.mistral.ai/api-keys/",
             false,
             true});
        add({"groq",
             "Groq",
             kAuthTypeApiKey,
             "https://api.groq.com/openai/v1",
             {"GROQ_API_KEY"},
             "https://console.groq.com/keys",
             false,
             true});
        add({"xai",
             "xAI (Grok)",
             kAuthTypeApiKey,
             "https://api.x.ai/v1",
             {"XAI_API_KEY"},
             "https://console.x.ai/",
             false,
             true});
        add({"zai",
             "Z.AI",
             kAuthTypeApiKey,
             "https://api.z.ai/api/paas/v4",
             {"ZAI_API_KEY"},
             "https://open.bigmodel.cn/usercenter/apikeys",
             false,
             true});
        add({"cohere",
             "Cohere",
             kAuthTypeApiKey,
             "https://api.cohere.ai/compatibility/v1",
             {"COHERE_API_KEY"},
             "https://dashboard.cohere.com/api-keys",
             false,
             true});
        add({"huggingface",
             "HuggingFace",
             kAuthTypeApiKey,
             "https://router.huggingface.co/v1",
             {"HF_TOKEN", "HUGGING_FACE_HUB_TOKEN"},
             "https://huggingface.co/settings/tokens",
             false,
             true});
        add({"togetherai",
             "Together AI",
             kAuthTypeApiKey,
             "https://api.together.xyz/v1",
             {"TOGETHER_API_KEY"},
             "https://api.together.ai/settings/api-keys",
             false,
             true});
        add({"fireworks",
             "Fireworks",
             kAuthTypeApiKey,
             "https://api.fireworks.ai/inference/v1",
             {"FIREWORKS_API_KEY"},
             "https://fireworks.ai/account/api-keys",
             false,
             true});
        add({"perplexity",
             "Perplexity",
             kAuthTypeApiKey,
             "https://api.perplexity.ai",
             {"PERPLEXITY_API_KEY"},
             "https://www.perplexity.ai/settings/api",
             false,
             true});
        add({"copilot",
             "GitHub Copilot",
             kAuthTypeOAuth,
             "",
             {"GITHUB_COPILOT_TOKEN"},
             "https://github.com/settings/copilot",
             true,
             false});
        add({"nous",
             "Nous Portal",
             kAuthTypeManaged,
             "https://api.nousresearch.com/v1",
             {"NOUS_API_KEY"},
             "https://portal.nousresearch.com/",
             true,
             false});
        return m;
    };
    static const auto m = build();
    return m;
}

const ProviderConfig* find_provider(const std::string& provider_id) {
    auto id = to_lower(strip(provider_id));
    if (id.empty()) return nullptr;
    const auto& reg = provider_registry();
    auto it = reg.find(id);
    if (it == reg.end()) return nullptr;
    return &it->second;
}

// ---------------------------------------------------------------------------
// is_provider_explicitly_configured.
// ---------------------------------------------------------------------------

bool is_provider_explicitly_configured(const std::string& provider_id,
                                       const std::string& config_provider,
                                       const EnvLookupFn& env_lookup) {
    auto normalized = to_lower(strip(provider_id));
    if (normalized.empty()) return false;

    // 1. auth.json active_provider match.
    try {
        auto active = get_active_provider();
        if (active) {
            auto a = to_lower(strip(*active));
            if (a == normalized) return true;
        }
    } catch (...) {}

    // 2. config.yaml model.provider match.
    if (!config_provider.empty()) {
        if (to_lower(strip(config_provider)) == normalized) return true;
    }

    // 3. Provider-specific env vars.
    const ProviderConfig* pc = find_provider(normalized);
    if (pc && pc->auth_type == kAuthTypeApiKey) {
        EnvLookupFn lookup = env_lookup ? env_lookup : default_env_lookup;
        const auto& implicit = implicit_env_vars();
        for (const auto& ev : pc->api_key_env_vars) {
            if (implicit.count(ev)) continue;
            if (has_usable_secret(lookup(ev))) return true;
        }
    }

    return false;
}

}  // namespace hermes::cli::auth_core
