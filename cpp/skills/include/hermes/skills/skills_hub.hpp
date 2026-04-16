// Skills Hub client — ported (partial) from Python tools/skills_hub.py.
//
// This header exposes the subset of hub surface area needed by the C++
// agent: discovery/search, install/upgrade, metadata cache, taps
// configuration, hub lock file tracking, trust levels, version pinning,
// and transitive dependency resolution.  The actual HTTP plumbing goes
// through hermes::llm transport — there is no direct libcurl dependency.
#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace hermes::llm {
class HttpTransport;
}

namespace hermes::skills {

// ---------------------------------------------------------------------------
// Data types
// ---------------------------------------------------------------------------

// Trust tier for a given skill source.  "builtin" = shipped in repo;
// "trusted" = signed by a known publisher or one of the curated taps;
// "community" = everything else (install requires explicit confirmation).
enum class TrustLevel { Builtin, Trusted, Community };

std::string to_string(TrustLevel t);
TrustLevel trust_level_from_string(std::string_view s);

struct HubSkillEntry {
    std::string name;
    std::string description;
    std::string version;
    std::string author;
    std::string repo_url;
    // Source adapter that produced the entry ("official", "github",
    // "clawhub", "claude-marketplace", "lobehub").
    std::string source;
    // Source-specific identifier (e.g. "openai/skills/skill-creator").
    std::string identifier;
    TrustLevel trust = TrustLevel::Community;
    std::vector<std::string> tags;
    // Dependencies listed in the skill manifest (may be empty).  Each
    // dependency is "<name>" or "<name>@<version>".
    std::vector<std::string> dependencies;
};

// Lightweight skill record returned by the remote Hub HTTP API.
// Unlike `HubSkillEntry` (which tracks source/identifier/trust for
// tap-based discovery) this type mirrors the REST `Skill` schema and
// is used exclusively by the token-authenticated HTTP surface.
struct SkillEntry {
    std::string name;
    std::string version;
    std::string description;
    std::string author;
    std::vector<std::string> tags;
    std::string download_url;  // tarball URL; empty when not provided
    std::uint64_t size_bytes = 0;
    std::string updated_at;    // ISO-8601 timestamp
};

// One record in `lock.json` — identifies a pinned, installed skill.
struct HubLockEntry {
    std::string name;
    std::string source;
    std::string identifier;
    std::string version;
    std::string content_hash;  // sha256 of concatenated bundle contents
    TrustLevel trust = TrustLevel::Community;
    std::chrono::system_clock::time_point installed_at{};
};

// A "tap" — a configured GitHub repository/path acting as a skill
// registry.  Mirrors `GitHubSource.DEFAULT_TAPS` plus user-added taps.
struct HubTap {
    std::string repo;  // e.g. "openai/skills"
    std::string path;  // e.g. "skills/"
    std::string branch = "main";
    TrustLevel trust = TrustLevel::Community;
};

// Result of a scan/verify pass over a downloaded bundle.
struct ScanReport {
    bool passed = false;
    std::vector<std::string> warnings;
    std::vector<std::string> errors;
};

// ---------------------------------------------------------------------------
// Hub paths helper (visible for tests).
// ---------------------------------------------------------------------------

struct HubPaths {
    std::filesystem::path skills_dir;
    std::filesystem::path hub_dir;
    std::filesystem::path lock_file;
    std::filesystem::path quarantine_dir;
    std::filesystem::path audit_log;
    std::filesystem::path taps_file;
    std::filesystem::path index_cache_dir;

    static HubPaths discover();  // honours $HOME / HERMES_HOME
    static HubPaths for_root(const std::filesystem::path& hermes_home);
};

// Ensure every hub directory in `paths` exists.  Returns true on success.
bool ensure_hub_paths(const HubPaths& paths);

// ---------------------------------------------------------------------------
// Index cache — keyed JSON blobs with TTL expiry.
// ---------------------------------------------------------------------------

class IndexCache {
public:
    explicit IndexCache(std::filesystem::path cache_dir,
                        std::chrono::seconds ttl = std::chrono::hours(1));

    // Returns the cached body if present and not expired.
    std::optional<std::string> get(const std::string& key) const;

    // Store `body` under `key`.
    void put(const std::string& key, const std::string& body);

    // Wipe all entries.
    void clear();

    // Remove only entries older than the configured TTL.
    std::size_t prune_expired() const;

    std::filesystem::path path_for(const std::string& key) const;

private:
    std::filesystem::path dir_;
    std::chrono::seconds ttl_;
};

// ---------------------------------------------------------------------------
// Lock file — JSON record of what's installed.
// ---------------------------------------------------------------------------

class HubLockFile {
public:
    explicit HubLockFile(std::filesystem::path path);

    // Load from disk (silently starts fresh if missing / corrupt).
    void load();
    // Persist to disk via atomic_io.
    bool save() const;

    void upsert(const HubLockEntry& entry);
    bool remove(const std::string& name);
    std::optional<HubLockEntry> get(const std::string& name) const;
    std::vector<HubLockEntry> all() const;
    bool contains(const std::string& name) const;

    // Count of currently tracked skills.
    std::size_t size() const;

private:
    std::filesystem::path path_;
    std::unordered_map<std::string, HubLockEntry> entries_;
};

// ---------------------------------------------------------------------------
// Taps — user-managed list of GitHub taps.
// ---------------------------------------------------------------------------

class TapStore {
public:
    explicit TapStore(std::filesystem::path taps_file);

    void load();
    bool save() const;

    const std::vector<HubTap>& taps() const { return taps_; }

    // Add a tap if not already present (by repo+path).  Returns true on
    // insertion, false if it was already present.
    bool add(const HubTap& tap);
    bool remove(const std::string& repo, const std::string& path);

    // The curated default taps (openai/skills, anthropics/skills, ...).
    static std::vector<HubTap> default_taps();

private:
    std::filesystem::path path_;
    std::vector<HubTap> taps_;
};

// ---------------------------------------------------------------------------
// Bundle + dependency helpers.
// ---------------------------------------------------------------------------

// Parse a "<name>@<version>" or "<name>" spec.
struct ParsedSpec {
    std::string name;
    std::string version;  // empty = "latest"
};
ParsedSpec parse_spec(std::string_view spec);

// Compare two semver-ish version strings (e.g. "1.2.3", "0.1"). Returns
// -1 / 0 / +1 — missing numeric parts are treated as 0, non-numeric
// parts fall back to lexicographic comparison.
int compare_versions(std::string_view a, std::string_view b);

// Returns true if `installed` satisfies the requested pin (empty /
// "latest" / "*" always satisfy, "<ver>" is exact match, ">=x.y" is
// range).
bool satisfies_pin(std::string_view installed, std::string_view pin);

// Produce the install order for a target skill, following
// dependency edges.  Cycles are broken (each name appears at most once).
std::vector<std::string> topo_order(
    const std::string& root,
    const std::function<std::vector<std::string>(const std::string&)>&
        deps_fn);

// SHA-256 of arbitrary bytes, lowercase hex.
std::string sha256_hex(std::string_view bytes);

// Compute content_hash for an installed skill directory.  Returns an
// empty string if `dir` doesn't exist.  The hash is
// sha256(sorted(relpath\0content)) — stable across platforms.
std::string hash_skill_dir(const std::filesystem::path& dir);

// ---------------------------------------------------------------------------
// SkillsHub — top-level API.
// ---------------------------------------------------------------------------

class SkillsHub {
public:
    SkillsHub();
    explicit SkillsHub(HubPaths paths);
    // Full constructor — inject a custom transport (for tests) and base
    // URL.  When transport is nullptr SkillsHub falls back to
    // hermes::llm::get_default_transport().
    SkillsHub(HubPaths paths,
              hermes::llm::HttpTransport* transport,
              std::string base_url);

    // Override the hub base URL (defaults to kDefaultHubBase).
    void set_base_url(std::string url);
    const std::string& base_url() const { return base_url_; }

    // Inject an HTTP transport (for tests).
    void set_transport(hermes::llm::HttpTransport* transport);

    const HubPaths& paths() const { return paths_; }
    HubLockFile& lock() { return lock_; }
    const HubLockFile& lock() const { return lock_; }
    TapStore& taps() { return taps_; }
    const TapStore& taps() const { return taps_; }
    IndexCache& cache() { return cache_; }

    // Returns the candidates matching `query`.  Empty result when HTTP
    // transport is unavailable or all taps fail.
    std::vector<HubSkillEntry> search(const std::string& query,
                                      std::size_t limit = 10);

    // Fetch full metadata for one skill (by name).  Consults the cache,
    // then falls through to the HTTP transport.
    std::optional<HubSkillEntry> get(const std::string& name);

    // Install a skill (and its transitive deps).  Returns true on full
    // success; partial installs are rolled back.  When `pin` is empty
    // the latest version is used.
    bool install(const std::string& name, const std::string& pin = "");

    // Remove an installed skill.  Returns false if the skill is not
    // tracked in the lock file or the directory can't be removed.
    bool uninstall(const std::string& name);

    // Upgrade to the latest version (falls back to install if not
    // previously installed).  Respects an optional pin.
    bool update(const std::string& name, const std::string& pin = "");

    // Verify an installed skill still matches its recorded content hash.
    // Returns false (and logs to audit log) if drift is detected.
    bool verify(const std::string& name);

    // Run all curated safety checks on `dir` — syntax probe, banned
    // patterns, manifest sanity.  Returns a ScanReport rather than a
    // bool so callers can surface warnings.
    ScanReport scan_bundle(const std::filesystem::path& dir) const;

    // Append one line to the audit log.  Each line is JSON-encoded with
    // an RFC3339 timestamp.
    void audit(const std::string& action,
               const std::string& name,
               const std::string& detail = "") const;

    // Compute/return the install order for a skill by consulting the
    // hub's dependency metadata.  Caller is expected to install entries
    // front-to-back.
    std::vector<std::string> resolve_install_order(
        const std::string& name);

    // Expose hub state as JSON (for /skills ls).
    std::string state_json() const;

    // Upload an installed skill bundle to the hub.  Serialises the
    // skill directory into a JSON envelope (relative path → base64
    // bytes) and POSTs it to `{base_url}/skills/{name}/upload` with a
    // Bearer token.  Returns true on HTTP 2xx.
    bool upload(const std::string& name, const std::string& token,
                std::string* error_out = nullptr);

    // ---------------------------------------------------------------
    // Token-authenticated HTTP surface (Stream B — remote Hub API).
    // These methods talk to `{base_url}/skills/...` directly via the
    // injected HttpTransport, attaching `Authorization: Bearer <token>`
    // when the token is non-empty.  They neither read nor mutate the
    // local lock file / quarantine — callers handle persistence.
    // Errors are surfaced via optional `error_out` strings.
    // ---------------------------------------------------------------

    // Pull one page of the hub catalogue.
    // GET {base}/skills?page=<page>&page_size=<page_size>
    std::vector<SkillEntry> list_all(const std::string& token,
                                     int page = 1,
                                     int page_size = 50,
                                     std::string* error_out = nullptr) const;

    // Search the hub:  GET {base}/skills/search?q=<query>
    std::vector<SkillEntry> search(const std::string& query,
                                   const std::string& token,
                                   std::string* error_out = nullptr) const;

    // Fetch metadata for one skill by name.
    // GET {base}/skills/<name>
    std::optional<SkillEntry> get(const std::string& name,
                                  const std::string& token,
                                  std::string* error_out = nullptr) const;

    // Download the skill tarball from `SkillEntry::download_url`, then
    // extract into `dest_root / name`.  Any pre-existing directory is
    // moved aside to `<name>.bak.<ts>` first.  On success returns the
    // installed path; on failure returns std::nullopt and fills
    // `error_out`.  On non-POSIX platforms this returns nullopt with a
    // "tar extract requires Stream C platform layer" error.
    std::optional<std::filesystem::path> install(
        const std::string& name,
        const std::filesystem::path& dest_root,
        const std::string& token,
        std::string* error_out = nullptr) const;

private:
    HubPaths paths_;
    HubLockFile lock_;
    TapStore taps_;
    mutable IndexCache cache_;
    hermes::llm::HttpTransport* transport_ = nullptr;
    std::string base_url_;

    // Internal HTTP fetch helper, records audit entries on failure.
    std::optional<std::string> http_get_(const std::string& url) const;

    // Download+stage a single skill to quarantine, returning its
    // staging dir.  Empty optional on failure.
    std::optional<std::filesystem::path> stage_(const HubSkillEntry& entry);

    // Promote a staged directory to the live skills dir (atomic
    // rename where possible).
    bool promote_(const std::filesystem::path& staging,
                  const std::string& name);
};

}  // namespace hermes::skills
