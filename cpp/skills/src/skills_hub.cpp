// Skills Hub client implementation.
//
// This file is deliberately long — it ports the runtime-relevant surface
// of tools/skills_hub.py (2775 LoC of Python) into one self-contained
// translation unit.  The network layer is pluggable via
// hermes::llm::HttpTransport so tests can exercise every code path with
// a FakeHttpTransport.
#include "hermes/skills/skills_hub.hpp"

#include "hermes/core/atomic_io.hpp"
#include "hermes/core/path.hpp"
#include "hermes/llm/llm_client.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <system_error>
#include <unordered_set>
#include <vector>

namespace hermes::skills {

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {

constexpr const char* kDefaultHubBase = "https://hermes-skills.hub/api";

std::string rfc3339_now() {
    auto now = std::chrono::system_clock::now();
    auto t = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
    return buf;
}

std::int64_t tp_to_ms(std::chrono::system_clock::time_point tp) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               tp.time_since_epoch())
        .count();
}

std::chrono::system_clock::time_point ms_to_tp(std::int64_t ms) {
    return std::chrono::system_clock::time_point{
        std::chrono::milliseconds{ms}};
}

std::string lower(std::string_view s) {
    std::string out(s);
    for (auto& c : out) c = static_cast<char>(std::tolower(c));
    return out;
}

bool is_ascii_alnum(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9');
}

// Transform an arbitrary cache key into a safe filename.
std::string safe_key(const std::string& key) {
    std::string out;
    out.reserve(key.size() + 8);
    for (char c : key) {
        if (is_ascii_alnum(c) || c == '-' || c == '_' || c == '.') {
            out.push_back(c);
        } else {
            char buf[4];
            std::snprintf(buf, sizeof(buf), "_%02x",
                          static_cast<unsigned char>(c));
            out += buf;
        }
    }
    if (out.empty()) out = "_empty";
    return out + ".json";
}

// ---- SHA-256 (RFC 6234 style).  Self-contained so we don't drag in
// OpenSSL.  Good enough for content-integrity hashing (not for
// cryptographic security vs. adversaries).
struct Sha256 {
    std::uint32_t h[8];
    std::uint64_t len = 0;
    unsigned char buf[64];
    std::size_t buf_len = 0;

    Sha256() { reset(); }
    void reset() {
        h[0] = 0x6a09e667; h[1] = 0xbb67ae85; h[2] = 0x3c6ef372;
        h[3] = 0xa54ff53a; h[4] = 0x510e527f; h[5] = 0x9b05688c;
        h[6] = 0x1f83d9ab; h[7] = 0x5be0cd19;
        len = 0; buf_len = 0;
    }
    static std::uint32_t rotr(std::uint32_t x, int n) {
        return (x >> n) | (x << (32 - n));
    }
    void compress(const unsigned char* chunk) {
        static const std::uint32_t K[64] = {
            0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,
            0x923f82a4,0xab1c5ed5,0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,
            0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,0xe49b69c1,0xefbe4786,
            0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
            0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,
            0x06ca6351,0x14292967,0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,
            0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,0xa2bfe8a1,0xa81a664b,
            0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
            0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,
            0x5b9cca4f,0x682e6ff3,0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,
            0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2};
        std::uint32_t w[64];
        for (int i = 0; i < 16; ++i) {
            w[i] = (std::uint32_t(chunk[i*4]) << 24) |
                   (std::uint32_t(chunk[i*4+1]) << 16) |
                   (std::uint32_t(chunk[i*4+2]) << 8) |
                   std::uint32_t(chunk[i*4+3]);
        }
        for (int i = 16; i < 64; ++i) {
            std::uint32_t s0 = rotr(w[i-15],7) ^ rotr(w[i-15],18) ^ (w[i-15]>>3);
            std::uint32_t s1 = rotr(w[i-2],17) ^ rotr(w[i-2],19) ^ (w[i-2]>>10);
            w[i] = w[i-16] + s0 + w[i-7] + s1;
        }
        std::uint32_t a=h[0],b=h[1],c=h[2],d=h[3],e=h[4],f=h[5],g=h[6],hh=h[7];
        for (int i = 0; i < 64; ++i) {
            std::uint32_t S1 = rotr(e,6) ^ rotr(e,11) ^ rotr(e,25);
            std::uint32_t ch = (e & f) ^ (~e & g);
            std::uint32_t t1 = hh + S1 + ch + K[i] + w[i];
            std::uint32_t S0 = rotr(a,2) ^ rotr(a,13) ^ rotr(a,22);
            std::uint32_t mj = (a & b) ^ (a & c) ^ (b & c);
            std::uint32_t t2 = S0 + mj;
            hh=g; g=f; f=e; e=d+t1; d=c; c=b; b=a; a=t1+t2;
        }
        h[0]+=a; h[1]+=b; h[2]+=c; h[3]+=d;
        h[4]+=e; h[5]+=f; h[6]+=g; h[7]+=hh;
    }
    void update(const void* data, std::size_t n) {
        const auto* p = static_cast<const unsigned char*>(data);
        len += n;
        if (buf_len) {
            std::size_t take = std::min<std::size_t>(64 - buf_len, n);
            std::memcpy(buf + buf_len, p, take);
            buf_len += take;
            p += take; n -= take;
            if (buf_len == 64) { compress(buf); buf_len = 0; }
        }
        while (n >= 64) { compress(p); p += 64; n -= 64; }
        if (n) { std::memcpy(buf, p, n); buf_len = n; }
    }
    std::string final_hex() {
        std::uint64_t bit_len = len * 8;
        unsigned char pad = 0x80;
        update(&pad, 1);
        unsigned char zero = 0;
        while (buf_len != 56) update(&zero, 1);
        unsigned char be[8];
        for (int i = 0; i < 8; ++i) be[i] = (bit_len >> (56 - 8*i)) & 0xff;
        update(be, 8);
        static const char* hex = "0123456789abcdef";
        std::string out; out.resize(64);
        for (int i = 0; i < 8; ++i) {
            for (int j = 0; j < 4; ++j) {
                unsigned char byte = (h[i] >> (24 - 8*j)) & 0xff;
                out[i*8 + j*2] = hex[byte >> 4];
                out[i*8 + j*2 + 1] = hex[byte & 0xf];
            }
        }
        return out;
    }
};

// Banned content patterns — the C++ scanner is intentionally shallow
// (the Python reference does a much deeper audit).  These substrings
// surface the most common footguns so install can warn.
const std::array<const char*, 6> kBannedSubstrings = {{
    "rm -rf /",
    "sudo rm",
    ":(){ :|:& };:",   // fork bomb
    "curl | sh",
    "eval $(curl",
    "/etc/shadow",
}};

}  // namespace

// ---------------------------------------------------------------------------
// TrustLevel helpers
// ---------------------------------------------------------------------------

std::string to_string(TrustLevel t) {
    switch (t) {
        case TrustLevel::Builtin: return "builtin";
        case TrustLevel::Trusted: return "trusted";
        case TrustLevel::Community: return "community";
    }
    return "community";
}

TrustLevel trust_level_from_string(std::string_view s) {
    if (s == "builtin") return TrustLevel::Builtin;
    if (s == "trusted") return TrustLevel::Trusted;
    return TrustLevel::Community;
}

// ---------------------------------------------------------------------------
// HubPaths
// ---------------------------------------------------------------------------

HubPaths HubPaths::for_root(const fs::path& root) {
    HubPaths p;
    p.skills_dir = root / "skills";
    p.hub_dir = p.skills_dir / ".hub";
    p.lock_file = p.hub_dir / "lock.json";
    p.quarantine_dir = p.hub_dir / "quarantine";
    p.audit_log = p.hub_dir / "audit.log";
    p.taps_file = p.hub_dir / "taps.json";
    p.index_cache_dir = p.hub_dir / "index-cache";
    return p;
}

HubPaths HubPaths::discover() {
    return for_root(hermes::core::path::get_hermes_home());
}

bool ensure_hub_paths(const HubPaths& paths) {
    std::error_code ec;
    fs::create_directories(paths.skills_dir, ec);
    fs::create_directories(paths.hub_dir, ec);
    fs::create_directories(paths.quarantine_dir, ec);
    fs::create_directories(paths.index_cache_dir, ec);
    return !ec;
}

// ---------------------------------------------------------------------------
// IndexCache
// ---------------------------------------------------------------------------

IndexCache::IndexCache(fs::path dir, std::chrono::seconds ttl)
    : dir_(std::move(dir)), ttl_(ttl) {}

fs::path IndexCache::path_for(const std::string& key) const {
    return dir_ / safe_key(key);
}

std::optional<std::string> IndexCache::get(const std::string& key) const {
    auto p = path_for(key);
    std::error_code ec;
    if (!fs::exists(p, ec)) return std::nullopt;
    auto ftime = fs::last_write_time(p, ec);
    if (ec) return std::nullopt;
    auto sctp = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
        ftime - decltype(ftime)::clock::now() +
        std::chrono::system_clock::now());
    auto age = std::chrono::system_clock::now() - sctp;
    if (age > ttl_) return std::nullopt;
    auto body = hermes::core::atomic_io::atomic_read(p);
    return body;
}

void IndexCache::put(const std::string& key, const std::string& body) {
    std::error_code ec;
    fs::create_directories(dir_, ec);
    hermes::core::atomic_io::atomic_write(path_for(key), body);
}

void IndexCache::clear() {
    std::error_code ec;
    if (!fs::exists(dir_, ec)) return;
    for (auto& entry : fs::directory_iterator(dir_, ec)) {
        fs::remove_all(entry.path(), ec);
    }
}

std::size_t IndexCache::prune_expired() const {
    std::size_t n = 0;
    std::error_code ec;
    if (!fs::exists(dir_, ec)) return 0;
    auto now_sys = std::chrono::system_clock::now();
    for (auto& entry : fs::directory_iterator(dir_, ec)) {
        auto ftime = fs::last_write_time(entry.path(), ec);
        if (ec) continue;
        auto sctp =
            std::chrono::time_point_cast<std::chrono::system_clock::duration>(
                ftime - decltype(ftime)::clock::now() + now_sys);
        if ((now_sys - sctp) > ttl_) {
            fs::remove(entry.path(), ec);
            if (!ec) ++n;
        }
    }
    return n;
}

// ---------------------------------------------------------------------------
// HubLockFile
// ---------------------------------------------------------------------------

HubLockFile::HubLockFile(fs::path path) : path_(std::move(path)) {}

void HubLockFile::load() {
    entries_.clear();
    auto raw = hermes::core::atomic_io::atomic_read(path_);
    if (!raw) return;
    auto root = json::parse(*raw, nullptr, false);
    if (root.is_discarded() || !root.is_object()) return;
    const char* key = root.contains("installed") ? "installed" : "skills";
    if (!root.contains(key) || !root[key].is_array()) return;
    for (const auto& item : root[key]) {
        HubLockEntry e;
        e.name = item.value("name", "");
        e.source = item.value("source", "");
        e.identifier = item.value("identifier", "");
        e.version = item.value("version", "");
        e.content_hash = item.value("content_hash", "");
        e.trust = trust_level_from_string(
            item.value("trust", std::string("community")));
        e.installed_at =
            ms_to_tp(item.value("installed_at", std::int64_t{0}));
        if (!e.name.empty()) entries_[e.name] = std::move(e);
    }
}

bool HubLockFile::save() const {
    json arr = json::array();
    for (const auto& [_, e] : entries_) {
        json j;
        j["name"] = e.name;
        j["source"] = e.source;
        j["identifier"] = e.identifier;
        j["version"] = e.version;
        j["content_hash"] = e.content_hash;
        j["trust"] = to_string(e.trust);
        j["installed_at"] = tp_to_ms(e.installed_at);
        arr.push_back(std::move(j));
    }
    json root;
    root["version"] = 1;
    root["installed"] = std::move(arr);
    std::error_code ec;
    fs::create_directories(path_.parent_path(), ec);
    return hermes::core::atomic_io::atomic_write(path_, root.dump(2));
}

void HubLockFile::upsert(const HubLockEntry& entry) {
    if (entry.name.empty()) return;
    entries_[entry.name] = entry;
}

bool HubLockFile::remove(const std::string& name) {
    return entries_.erase(name) > 0;
}

std::optional<HubLockEntry> HubLockFile::get(const std::string& name) const {
    auto it = entries_.find(name);
    if (it == entries_.end()) return std::nullopt;
    return it->second;
}

std::vector<HubLockEntry> HubLockFile::all() const {
    std::vector<HubLockEntry> out;
    out.reserve(entries_.size());
    for (const auto& [_, e] : entries_) out.push_back(e);
    std::sort(out.begin(), out.end(),
              [](const auto& a, const auto& b) { return a.name < b.name; });
    return out;
}

bool HubLockFile::contains(const std::string& name) const {
    return entries_.count(name) > 0;
}

std::size_t HubLockFile::size() const { return entries_.size(); }

// ---------------------------------------------------------------------------
// TapStore
// ---------------------------------------------------------------------------

std::vector<HubTap> TapStore::default_taps() {
    return {
        {"openai/skills", "skills/", "main", TrustLevel::Trusted},
        {"anthropics/skills", "skills/", "main", TrustLevel::Trusted},
        {"VoltAgent/awesome-agent-skills", "skills/", "main",
         TrustLevel::Community},
        {"garrytan/gstack", "", "main", TrustLevel::Community},
    };
}

TapStore::TapStore(fs::path path)
    : path_(std::move(path)), taps_(default_taps()) {}

void TapStore::load() {
    auto raw = hermes::core::atomic_io::atomic_read(path_);
    if (!raw) {
        taps_ = default_taps();
        return;
    }
    auto root = json::parse(*raw, nullptr, false);
    if (root.is_discarded() || !root.is_array()) {
        taps_ = default_taps();
        return;
    }
    taps_.clear();
    for (const auto& item : root) {
        HubTap t;
        t.repo = item.value("repo", "");
        t.path = item.value("path", "");
        t.branch = item.value("branch", std::string("main"));
        t.trust = trust_level_from_string(
            item.value("trust", std::string("community")));
        if (!t.repo.empty()) taps_.push_back(std::move(t));
    }
    if (taps_.empty()) taps_ = default_taps();
}

bool TapStore::save() const {
    json arr = json::array();
    for (const auto& t : taps_) {
        json j;
        j["repo"] = t.repo;
        j["path"] = t.path;
        j["branch"] = t.branch;
        j["trust"] = to_string(t.trust);
        arr.push_back(std::move(j));
    }
    std::error_code ec;
    fs::create_directories(path_.parent_path(), ec);
    return hermes::core::atomic_io::atomic_write(path_, arr.dump(2));
}

bool TapStore::add(const HubTap& tap) {
    for (const auto& existing : taps_) {
        if (existing.repo == tap.repo && existing.path == tap.path) {
            return false;
        }
    }
    taps_.push_back(tap);
    return true;
}

bool TapStore::remove(const std::string& repo, const std::string& path) {
    auto before = taps_.size();
    taps_.erase(
        std::remove_if(taps_.begin(), taps_.end(),
                       [&](const HubTap& t) {
                           return t.repo == repo && t.path == path;
                       }),
        taps_.end());
    return taps_.size() != before;
}

// ---------------------------------------------------------------------------
// Spec parsing / version comparison
// ---------------------------------------------------------------------------

ParsedSpec parse_spec(std::string_view spec) {
    ParsedSpec out;
    auto at = spec.find('@');
    if (at == std::string_view::npos) {
        out.name = std::string(spec);
    } else {
        out.name = std::string(spec.substr(0, at));
        out.version = std::string(spec.substr(at + 1));
    }
    // Trim whitespace.
    auto trim = [](std::string& s) {
        std::size_t a = 0;
        while (a < s.size() &&
               std::isspace(static_cast<unsigned char>(s[a]))) ++a;
        std::size_t b = s.size();
        while (b > a && std::isspace(static_cast<unsigned char>(s[b-1]))) --b;
        s = s.substr(a, b - a);
    };
    trim(out.name);
    trim(out.version);
    return out;
}

namespace {

std::vector<std::string> split_dots(std::string_view s) {
    std::vector<std::string> out;
    std::size_t start = 0;
    for (std::size_t i = 0; i <= s.size(); ++i) {
        if (i == s.size() || s[i] == '.') {
            out.emplace_back(s.substr(start, i - start));
            start = i + 1;
        }
    }
    return out;
}

int cmp_component(const std::string& a, const std::string& b) {
    auto is_num = [](const std::string& s) {
        if (s.empty()) return false;
        for (char c : s) if (c < '0' || c > '9') return false;
        return true;
    };
    if (is_num(a) && is_num(b)) {
        // Compare by integer value — strip leading zeros.
        std::size_t ai = a.find_first_not_of('0');
        std::size_t bi = b.find_first_not_of('0');
        std::string aa = (ai == std::string::npos) ? "0" : a.substr(ai);
        std::string bb = (bi == std::string::npos) ? "0" : b.substr(bi);
        if (aa.size() != bb.size()) return aa.size() < bb.size() ? -1 : 1;
        if (aa < bb) return -1;
        if (aa > bb) return 1;
        return 0;
    }
    if (a < b) return -1;
    if (a > b) return 1;
    return 0;
}

}  // namespace

int compare_versions(std::string_view a, std::string_view b) {
    auto av = split_dots(a);
    auto bv = split_dots(b);
    std::size_t n = std::max(av.size(), bv.size());
    for (std::size_t i = 0; i < n; ++i) {
        const std::string& x = i < av.size() ? av[i] : std::string("0");
        const std::string& y = i < bv.size() ? bv[i] : std::string("0");
        int c = cmp_component(x, y);
        if (c != 0) return c;
    }
    return 0;
}

bool satisfies_pin(std::string_view installed, std::string_view pin) {
    if (pin.empty() || pin == "*" || pin == "latest") return true;
    if (pin.size() >= 2 && pin.substr(0, 2) == ">=") {
        return compare_versions(installed, pin.substr(2)) >= 0;
    }
    if (pin.size() >= 2 && pin.substr(0, 2) == "<=") {
        return compare_versions(installed, pin.substr(2)) <= 0;
    }
    if (!pin.empty() && pin[0] == '>') {
        return compare_versions(installed, pin.substr(1)) > 0;
    }
    if (!pin.empty() && pin[0] == '<') {
        return compare_versions(installed, pin.substr(1)) < 0;
    }
    if (!pin.empty() && pin[0] == '=') {
        return compare_versions(installed, pin.substr(1)) == 0;
    }
    return installed == pin;
}

std::vector<std::string> topo_order(
    const std::string& root,
    const std::function<std::vector<std::string>(const std::string&)>&
        deps_fn) {
    std::vector<std::string> order;
    std::unordered_set<std::string> visited;
    std::unordered_set<std::string> on_stack;

    std::function<void(const std::string&)> visit =
        [&](const std::string& node) {
            if (visited.count(node)) return;
            if (on_stack.count(node)) return;  // break cycle
            on_stack.insert(node);
            for (const auto& dep : deps_fn(node)) {
                auto parsed = parse_spec(dep);
                if (!parsed.name.empty()) visit(parsed.name);
            }
            on_stack.erase(node);
            visited.insert(node);
            order.push_back(node);
        };
    visit(root);
    return order;
}

std::string sha256_hex(std::string_view bytes) {
    Sha256 h;
    h.update(bytes.data(), bytes.size());
    return h.final_hex();
}

std::string hash_skill_dir(const fs::path& dir) {
    std::error_code ec;
    if (!fs::exists(dir, ec) || !fs::is_directory(dir, ec)) return "";
    struct Entry { std::string rel; fs::path abs; };
    std::vector<Entry> entries;
    for (auto& p : fs::recursive_directory_iterator(dir, ec)) {
        if (ec) break;
        if (!p.is_regular_file()) continue;
        auto rel = fs::relative(p.path(), dir, ec).generic_string();
        if (ec) continue;
        entries.push_back({rel, p.path()});
    }
    std::sort(entries.begin(), entries.end(),
              [](const Entry& a, const Entry& b) { return a.rel < b.rel; });
    Sha256 h;
    for (const auto& e : entries) {
        h.update(e.rel.data(), e.rel.size());
        char sep = '\0';
        h.update(&sep, 1);
        std::ifstream ifs(e.abs, std::ios::binary);
        char buf[4096];
        while (ifs) {
            ifs.read(buf, sizeof(buf));
            auto n = ifs.gcount();
            if (n > 0) h.update(buf, static_cast<std::size_t>(n));
        }
        char nl = '\n';
        h.update(&nl, 1);
    }
    return h.final_hex();
}

// ---------------------------------------------------------------------------
// SkillsHub
// ---------------------------------------------------------------------------

SkillsHub::SkillsHub() : SkillsHub(HubPaths::discover()) {}

SkillsHub::SkillsHub(HubPaths paths)
    : SkillsHub(std::move(paths), nullptr, kDefaultHubBase) {}

SkillsHub::SkillsHub(HubPaths paths,
                     hermes::llm::HttpTransport* transport,
                     std::string base_url)
    : paths_(std::move(paths)),
      lock_(paths_.lock_file),
      taps_(paths_.taps_file),
      cache_(paths_.index_cache_dir),
      transport_(transport),
      base_url_(std::move(base_url)) {
    ensure_hub_paths(paths_);
    lock_.load();
    taps_.load();
}

void SkillsHub::set_base_url(std::string url) { base_url_ = std::move(url); }
void SkillsHub::set_transport(hermes::llm::HttpTransport* t) { transport_ = t; }

std::optional<std::string> SkillsHub::http_get_(const std::string& url) const {
    auto* transport = transport_ ? transport_
                                 : hermes::llm::get_default_transport();
    if (!transport) return std::nullopt;
    std::unordered_map<std::string, std::string> headers;
    headers["Accept"] = "application/json";
    headers["User-Agent"] = "hermes-agent-cpp/0.1 (+skills-hub)";
    try {
        auto resp = transport->get(url, headers);
        if (resp.status_code < 200 || resp.status_code >= 300)
            return std::nullopt;
        return resp.body;
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

namespace {

HubSkillEntry parse_entry(const json& j) {
    HubSkillEntry e;
    e.name = j.value("name", "");
    e.description = j.value("description", "");
    e.version = j.value("version", "");
    e.author = j.value("author", "");
    e.repo_url = j.value("repo_url", "");
    e.source = j.value("source", "");
    e.identifier = j.value("identifier", "");
    e.trust = trust_level_from_string(j.value("trust", std::string("community")));
    if (j.contains("tags") && j["tags"].is_array()) {
        for (const auto& t : j["tags"]) {
            if (t.is_string()) e.tags.push_back(t.get<std::string>());
        }
    }
    if (j.contains("dependencies") && j["dependencies"].is_array()) {
        for (const auto& d : j["dependencies"]) {
            if (d.is_string()) e.dependencies.push_back(d.get<std::string>());
        }
    }
    return e;
}

}  // namespace

std::vector<HubSkillEntry> SkillsHub::search(const std::string& query,
                                             std::size_t limit) {
    const std::string cache_key = "search:" + query + ":" + std::to_string(limit);
    std::string body;
    if (auto cached = cache_.get(cache_key)) {
        body = *cached;
    } else {
        auto fetched = http_get_(base_url_ + "/skills?q=" + query +
                                 "&limit=" + std::to_string(limit));
        if (!fetched) {
            audit("search.failed", query, "transport unavailable");
            return {};
        }
        body = *fetched;
        cache_.put(cache_key, body);
    }
    auto root = json::parse(body, nullptr, false);
    if (root.is_discarded()) return {};
    std::vector<HubSkillEntry> out;
    if (root.is_array()) {
        for (const auto& item : root) out.push_back(parse_entry(item));
    } else if (root.is_object() && root.contains("results") &&
               root["results"].is_array()) {
        for (const auto& item : root["results"]) out.push_back(parse_entry(item));
    }
    // Client-side filter — if the hub returned everything, narrow to
    // substring matches.
    if (!query.empty()) {
        auto ql = lower(query);
        out.erase(std::remove_if(out.begin(), out.end(),
                                 [&](const HubSkillEntry& e) {
                                     return lower(e.name).find(ql) ==
                                                std::string::npos &&
                                            lower(e.description).find(ql) ==
                                                std::string::npos;
                                 }),
                  out.end());
    }
    if (out.size() > limit) out.resize(limit);
    return out;
}

std::optional<HubSkillEntry> SkillsHub::get(const std::string& name) {
    const std::string cache_key = "skill:" + name;
    std::string body;
    if (auto cached = cache_.get(cache_key)) {
        body = *cached;
    } else {
        auto fetched = http_get_(base_url_ + "/skills/" + name);
        if (!fetched) {
            audit("get.failed", name, "transport unavailable");
            return std::nullopt;
        }
        body = *fetched;
        cache_.put(cache_key, body);
    }
    auto root = json::parse(body, nullptr, false);
    if (root.is_discarded() || !root.is_object()) return std::nullopt;
    return parse_entry(root);
}

std::vector<std::string> SkillsHub::resolve_install_order(
    const std::string& name) {
    // Build a closure that fetches deps on demand and caches them.
    std::unordered_map<std::string, std::vector<std::string>> dep_cache;
    auto deps_fn = [&](const std::string& n) -> std::vector<std::string> {
        auto it = dep_cache.find(n);
        if (it != dep_cache.end()) return it->second;
        auto entry = get(n);
        std::vector<std::string> deps;
        if (entry) deps = entry->dependencies;
        dep_cache[n] = deps;
        return deps;
    };
    return topo_order(name, deps_fn);
}

std::optional<fs::path> SkillsHub::stage_(const HubSkillEntry& entry) {
    if (entry.name.empty()) return std::nullopt;
    auto staging = paths_.quarantine_dir / entry.name;
    std::error_code ec;
    fs::remove_all(staging, ec);
    fs::create_directories(staging, ec);
    if (ec) return std::nullopt;

    // Write the manifest itself (always safe).
    json manifest;
    manifest["name"] = entry.name;
    manifest["description"] = entry.description;
    manifest["version"] = entry.version;
    manifest["author"] = entry.author;
    manifest["source"] = entry.source;
    manifest["identifier"] = entry.identifier;
    manifest["trust"] = to_string(entry.trust);
    manifest["tags"] = entry.tags;
    manifest["dependencies"] = entry.dependencies;
    hermes::core::atomic_io::atomic_write(staging / "metadata.json",
                                          manifest.dump(2));

    // Fetch SKILL.md when we have a repo_url — best effort.
    if (!entry.repo_url.empty()) {
        if (auto body = http_get_(entry.repo_url + "/SKILL.md")) {
            hermes::core::atomic_io::atomic_write(staging / "SKILL.md", *body);
        }
    }
    return staging;
}

bool SkillsHub::promote_(const fs::path& staging, const std::string& name) {
    auto dest = paths_.skills_dir / name;
    std::error_code ec;
    fs::remove_all(dest, ec);
    fs::create_directories(dest.parent_path(), ec);
    fs::rename(staging, dest, ec);
    if (ec) {
        // Fallback to copy+remove when rename crosses devices.
        fs::copy(staging, dest,
                 fs::copy_options::recursive |
                     fs::copy_options::overwrite_existing,
                 ec);
        fs::remove_all(staging);
    }
    return !ec;
}

bool SkillsHub::install(const std::string& name, const std::string& pin) {
    auto order = resolve_install_order(name);
    if (order.empty()) {
        audit("install.noop", name, "not found");
        return false;
    }
    std::vector<std::string> installed_now;
    for (const auto& dep_name : order) {
        // If already satisfied, skip.
        if (auto existing = lock_.get(dep_name)) {
            if (dep_name != name || satisfies_pin(existing->version, pin)) {
                continue;
            }
        }
        auto entry = get(dep_name);
        if (!entry) {
            audit("install.failed", dep_name, "metadata fetch failed");
            // Roll back anything we added this run.
            for (const auto& rolled : installed_now) uninstall(rolled);
            return false;
        }
        if (dep_name == name && !pin.empty() &&
            !satisfies_pin(entry->version, pin)) {
            audit("install.failed", dep_name,
                  "latest=" + entry->version + " does not match pin=" + pin);
            for (const auto& rolled : installed_now) uninstall(rolled);
            return false;
        }
        auto staging = stage_(*entry);
        if (!staging) {
            audit("install.failed", dep_name, "stage failed");
            for (const auto& rolled : installed_now) uninstall(rolled);
            return false;
        }
        ScanReport report = scan_bundle(*staging);
        if (!report.passed) {
            audit("install.failed", dep_name, "scan failed");
            std::error_code ec;
            fs::remove_all(*staging, ec);
            for (const auto& rolled : installed_now) uninstall(rolled);
            return false;
        }
        if (!promote_(*staging, dep_name)) {
            audit("install.failed", dep_name, "promote failed");
            for (const auto& rolled : installed_now) uninstall(rolled);
            return false;
        }
        HubLockEntry lock_entry;
        lock_entry.name = entry->name;
        lock_entry.source = entry->source;
        lock_entry.identifier = entry->identifier;
        lock_entry.version = entry->version;
        lock_entry.trust = entry->trust;
        lock_entry.installed_at = std::chrono::system_clock::now();
        lock_entry.content_hash = hash_skill_dir(paths_.skills_dir / dep_name);
        lock_.upsert(lock_entry);
        installed_now.push_back(dep_name);
        audit("install.ok", dep_name, "version=" + entry->version);
    }
    lock_.save();
    return true;
}

bool SkillsHub::uninstall(const std::string& name) {
    if (!lock_.contains(name)) {
        audit("uninstall.skip", name, "not tracked");
        return false;
    }
    auto dir = paths_.skills_dir / name;
    // Safety: confirm the path is under skills_dir.
    auto canonical_base =
        fs::weakly_canonical(paths_.skills_dir).generic_string();
    auto canonical_dir = fs::weakly_canonical(dir).generic_string();
    if (canonical_dir.rfind(canonical_base, 0) != 0) {
        audit("uninstall.failed", name, "path escape");
        return false;
    }
    std::error_code ec;
    fs::remove_all(dir, ec);
    lock_.remove(name);
    lock_.save();
    audit("uninstall.ok", name, "");
    return !ec;
}

bool SkillsHub::update(const std::string& name, const std::string& pin) {
    auto existing = lock_.get(name);
    auto entry = get(name);
    if (!entry) {
        audit("update.failed", name, "metadata fetch failed");
        return false;
    }
    if (existing && compare_versions(existing->version, entry->version) >= 0 &&
        (pin.empty() || satisfies_pin(existing->version, pin))) {
        audit("update.noop", name, "already current");
        return true;
    }
    if (existing) uninstall(name);
    return install(name, pin);
}

bool SkillsHub::verify(const std::string& name) {
    auto lock_entry = lock_.get(name);
    if (!lock_entry) return false;
    auto dir = paths_.skills_dir / name;
    auto current = hash_skill_dir(dir);
    if (current.empty() || current != lock_entry->content_hash) {
        audit("verify.drift", name,
              "expected=" + lock_entry->content_hash + " got=" + current);
        return false;
    }
    return true;
}

ScanReport SkillsHub::scan_bundle(const fs::path& dir) const {
    ScanReport report;
    report.passed = true;
    std::error_code ec;
    if (!fs::exists(dir, ec)) {
        report.passed = false;
        report.errors.push_back("staging directory missing");
        return report;
    }
    // Look for banned substrings in all regular files.
    for (auto& entry : fs::recursive_directory_iterator(dir, ec)) {
        if (ec) break;
        if (!entry.is_regular_file()) continue;
        std::ifstream ifs(entry.path(), std::ios::binary);
        std::ostringstream oss;
        oss << ifs.rdbuf();
        auto body = oss.str();
        for (const auto* banned : kBannedSubstrings) {
            if (body.find(banned) != std::string::npos) {
                report.passed = false;
                report.errors.push_back(
                    std::string("banned pattern '") + banned +
                    "' in " + entry.path().filename().string());
            }
        }
        // Heuristic warning: curl|bash chains.
        if (body.find("curl ") != std::string::npos &&
            body.find("| bash") != std::string::npos) {
            report.warnings.push_back(
                std::string("curl|bash chain in ") +
                entry.path().filename().string());
        }
    }
    return report;
}

void SkillsHub::audit(const std::string& action,
                      const std::string& name,
                      const std::string& detail) const {
    json j;
    j["ts"] = rfc3339_now();
    j["action"] = action;
    j["name"] = name;
    j["detail"] = detail;
    std::error_code ec;
    fs::create_directories(paths_.audit_log.parent_path(), ec);
    std::ofstream ofs(paths_.audit_log, std::ios::app);
    if (ofs) ofs << j.dump() << "\n";
}

std::string SkillsHub::state_json() const {
    json root;
    root["base_url"] = base_url_;
    root["skills_dir"] = paths_.skills_dir.string();
    json skills = json::array();
    for (const auto& e : lock_.all()) {
        json j;
        j["name"] = e.name;
        j["version"] = e.version;
        j["source"] = e.source;
        j["trust"] = to_string(e.trust);
        j["content_hash"] = e.content_hash;
        skills.push_back(std::move(j));
    }
    root["installed"] = std::move(skills);
    json taps = json::array();
    for (const auto& t : taps_.taps()) {
        json j;
        j["repo"] = t.repo;
        j["path"] = t.path;
        j["branch"] = t.branch;
        j["trust"] = to_string(t.trust);
        taps.push_back(std::move(j));
    }
    root["taps"] = std::move(taps);
    return root.dump(2);
}

}  // namespace hermes::skills
