// OpenClaw -> Hermes migration implementation.
#include "hermes/cli/claw_migrate.hpp"

#include "hermes/core/path.hpp"

#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <system_error>
#include <unordered_set>

namespace hermes::cli::claw {

namespace fs = std::filesystem;

namespace {

fs::path home_dir() {
    if (const char* h = std::getenv("HOME")) return fs::path(h);
    return fs::path("/");
}

std::string read_file(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return {};
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

bool write_file(const fs::path& p, const std::string& content) {
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    std::ofstream out(p, std::ios::binary);
    if (!out) return false;
    out << content;
    return out.good();
}

bool append_file(const fs::path& p, const std::string& content) {
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    std::ofstream out(p, std::ios::binary | std::ios::app);
    if (!out) return false;
    out << content;
    return out.good();
}

bool preset_includes_secrets(const std::string& preset) {
    return preset != "no-secrets";
}

bool preset_includes_workspace(const std::string& preset) {
    return preset == "full" || preset == "user-data";
}

}  // namespace

const std::vector<std::string>& api_key_allowlist() {
    static const std::vector<std::string> keys = {
        "TELEGRAM_BOT_TOKEN", "OPENROUTER_API_KEY", "OPENAI_API_KEY",
        "ANTHROPIC_API_KEY",  "ELEVENLABS_API_KEY",
    };
    return keys;
}

fs::path resolve_hermes_home(const MigrateOptions& opts) {
    if (!opts.hermes_home_override.empty()) return opts.hermes_home_override;
    return hermes::core::path::get_hermes_home();
}

fs::path resolve_openclaw_dir(const MigrateOptions& opts) {
    if (!opts.openclaw_dir.empty()) return opts.openclaw_dir;
    return home_dir() / ".openclaw";
}

// ── SOUL.md ─────────────────────────────────────────────────────────────

void migrate_soul(const MigrateOptions& opts, MigrationResult& result) {
    auto src = resolve_openclaw_dir(opts) / "SOUL.md";
    auto dst = resolve_hermes_home(opts) / "SOUL.md";
    if (!fs::exists(src)) {
        result.skipped.push_back("SOUL.md (source missing)");
        return;
    }
    if (fs::exists(dst) && !opts.overwrite) {
        result.skipped.push_back("SOUL.md (exists; use --overwrite)");
        return;
    }
    if (opts.dry_run) {
        result.imported.push_back("SOUL.md (dry-run)");
        ++result.item_count;
        return;
    }
    if (write_file(dst, read_file(src))) {
        result.imported.push_back("SOUL.md");
        ++result.item_count;
    } else {
        result.errors.push_back("SOUL.md: write failed");
    }
}

// ── MEMORY.md + USER.md ─────────────────────────────────────────────────

namespace {

void migrate_one_memory_file(const fs::path& src, const fs::path& dst,
                             const MigrateOptions& opts,
                             const std::string& label,
                             MigrationResult& result) {
    if (!fs::exists(src)) {
        result.skipped.push_back(label + " (source missing)");
        return;
    }
    auto content = read_file(src);
    if (content.empty()) {
        result.skipped.push_back(label + " (empty)");
        return;
    }
    if (opts.dry_run) {
        result.imported.push_back(label + " (dry-run)");
        ++result.item_count;
        return;
    }
    // Append, separated by the § section-sign memory separator, so
    // existing entries are preserved.
    std::string payload;
    if (fs::exists(dst)) payload += "\n\xC2\xA7\n";  // UTF-8 §
    payload += content;
    if (append_file(dst, payload)) {
        result.imported.push_back(label);
        ++result.item_count;
    } else {
        result.errors.push_back(label + ": append failed");
    }
}

}  // namespace

void migrate_memories(const MigrateOptions& opts, MigrationResult& result) {
    auto oc = resolve_openclaw_dir(opts);
    auto home = resolve_hermes_home(opts);
    migrate_one_memory_file(oc / "MEMORY.md",
                            home / "memories" / "MEMORY.md", opts, "MEMORY.md",
                            result);
    migrate_one_memory_file(oc / "USER.md",
                            home / "memories" / "USER.md", opts, "USER.md",
                            result);
}

// ── Skills ──────────────────────────────────────────────────────────────

void migrate_skills(const MigrateOptions& opts, MigrationResult& result) {
    auto src = resolve_openclaw_dir(opts) / "skills";
    auto dst = resolve_hermes_home(opts) / "skills" / "openclaw-imports";
    if (!fs::exists(src) || !fs::is_directory(src)) {
        result.skipped.push_back("skills (source missing)");
        return;
    }
    std::error_code ec;
    int count = 0;
    for (const auto& entry : fs::recursive_directory_iterator(src, ec)) {
        if (!entry.is_regular_file()) continue;
        auto rel = fs::relative(entry.path(), src);
        auto out_path = dst / rel;
        if (fs::exists(out_path) && !opts.overwrite) {
            result.skipped.push_back("skills/" + rel.string() +
                                     " (exists)");
            continue;
        }
        if (opts.dry_run) {
            ++count;
            continue;
        }
        if (write_file(out_path, read_file(entry.path()))) {
            ++count;
        } else {
            result.errors.push_back("skills/" + rel.string() + ": failed");
        }
    }
    if (count > 0) {
        result.imported.push_back("skills (" + std::to_string(count) + ")");
        result.item_count += count;
    }
}

// ── Command allowlist ───────────────────────────────────────────────────

void migrate_command_allowlist(const MigrateOptions& opts,
                               MigrationResult& result) {
    auto src = resolve_openclaw_dir(opts) / "config.yaml";
    auto dst = resolve_hermes_home(opts) / "config.yaml";
    if (!fs::exists(src)) {
        result.skipped.push_back("config.yaml (source missing)");
        return;
    }
    auto src_content = read_file(src);
    // Extract approval-related lines (best-effort YAML slice).
    std::istringstream in(src_content);
    std::string line;
    std::string extracted;
    bool in_approval = false;
    while (std::getline(in, line)) {
        if (line.find("approval") != std::string::npos ||
            line.find("allow_patterns") != std::string::npos ||
            line.find("allowed_commands") != std::string::npos) {
            in_approval = true;
            extracted += line + "\n";
            continue;
        }
        if (in_approval) {
            if (!line.empty() && line[0] == ' ') {
                extracted += line + "\n";
            } else {
                in_approval = false;
            }
        }
    }
    if (extracted.empty()) {
        result.skipped.push_back("command allowlist (none found)");
        return;
    }
    if (opts.dry_run) {
        result.imported.push_back("command allowlist (dry-run)");
        ++result.item_count;
        return;
    }
    std::string header = "\n# --- migrated from openclaw ---\n";
    if (append_file(dst, header + extracted)) {
        result.imported.push_back("command allowlist");
        ++result.item_count;
    } else {
        result.errors.push_back("command allowlist: append failed");
    }
}

// ── Messaging ────────────────────────────────────────────────────────────

void migrate_messaging(const MigrateOptions& opts, MigrationResult& result) {
    auto src = resolve_openclaw_dir(opts) / "messaging.yaml";
    auto dst = resolve_hermes_home(opts) / "messaging.yaml";
    if (!fs::exists(src)) {
        result.skipped.push_back("messaging.yaml (source missing)");
        return;
    }
    if (fs::exists(dst) && !opts.overwrite) {
        result.skipped.push_back("messaging.yaml (exists)");
        return;
    }
    if (opts.dry_run) {
        result.imported.push_back("messaging.yaml (dry-run)");
        ++result.item_count;
        return;
    }
    if (write_file(dst, read_file(src))) {
        result.imported.push_back("messaging.yaml");
        ++result.item_count;
    } else {
        result.errors.push_back("messaging.yaml: failed");
    }
}

// ── API keys ────────────────────────────────────────────────────────────

void migrate_api_keys(const MigrateOptions& opts, MigrationResult& result) {
    if (!preset_includes_secrets(opts.preset)) {
        result.skipped.push_back("api keys (preset=" + opts.preset + ")");
        return;
    }
    auto src = resolve_openclaw_dir(opts) / ".env";
    auto dst = resolve_hermes_home(opts) / ".env";
    if (!fs::exists(src)) {
        result.skipped.push_back(".env (source missing)");
        return;
    }
    std::unordered_set<std::string> allow;
    for (const auto& k : api_key_allowlist()) allow.insert(k);

    std::istringstream in(read_file(src));
    std::string line;
    std::string filtered;
    int n = 0;
    while (std::getline(in, line)) {
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        auto key = line.substr(0, eq);
        // Strip leading "export " and whitespace.
        const std::string prefix = "export ";
        if (key.rfind(prefix, 0) == 0) key = key.substr(prefix.size());
        while (!key.empty() && (key.front() == ' ' || key.front() == '\t'))
            key.erase(key.begin());
        while (!key.empty() && (key.back() == ' ' || key.back() == '\t'))
            key.pop_back();
        if (allow.count(key)) {
            filtered += line + "\n";
            ++n;
        }
    }
    if (n == 0) {
        result.skipped.push_back(".env (no allowlisted keys)");
        return;
    }
    if (opts.dry_run) {
        result.imported.push_back(".env (" + std::to_string(n) +
                                  " keys, dry-run)");
        result.item_count += n;
        return;
    }
    if (append_file(dst, filtered)) {
        result.imported.push_back(".env (" + std::to_string(n) + " keys)");
        result.item_count += n;
    } else {
        result.errors.push_back(".env: append failed");
    }
}

// ── TTS assets ──────────────────────────────────────────────────────────

void migrate_tts_assets(const MigrateOptions& opts, MigrationResult& result) {
    auto src = resolve_openclaw_dir(opts) / "workspace" / "audio";
    auto dst = resolve_hermes_home(opts) / "cache" / "audio";
    if (!fs::exists(src) || !fs::is_directory(src)) {
        result.skipped.push_back("tts assets (source missing)");
        return;
    }
    std::error_code ec;
    int count = 0;
    for (const auto& entry : fs::recursive_directory_iterator(src, ec)) {
        if (!entry.is_regular_file()) continue;
        auto rel = fs::relative(entry.path(), src);
        auto out_path = dst / rel;
        if (fs::exists(out_path) && !opts.overwrite) {
            continue;
        }
        if (opts.dry_run) {
            ++count;
            continue;
        }
        std::error_code cec;
        fs::create_directories(out_path.parent_path(), cec);
        fs::copy_file(entry.path(), out_path,
                      fs::copy_options::overwrite_existing, cec);
        if (!cec) ++count;
    }
    if (count > 0) {
        result.imported.push_back("tts assets (" + std::to_string(count) +
                                  ")");
        result.item_count += count;
    } else {
        result.skipped.push_back("tts assets (nothing to copy)");
    }
}

// ── AGENTS.md (workspace target) ────────────────────────────────────────

void migrate_agents_md(const MigrateOptions& opts, MigrationResult& result) {
    if (opts.workspace_target.empty()) {
        result.skipped.push_back("AGENTS.md (no --workspace-target)");
        return;
    }
    auto src = resolve_openclaw_dir(opts) / "AGENTS.md";
    if (!fs::exists(src)) {
        result.skipped.push_back("AGENTS.md (source missing)");
        return;
    }
    auto dst = opts.workspace_target / "AGENTS.md";
    if (fs::exists(dst) && !opts.overwrite) {
        result.skipped.push_back("AGENTS.md (target exists)");
        return;
    }
    if (opts.dry_run) {
        result.imported.push_back("AGENTS.md (dry-run)");
        ++result.item_count;
        return;
    }
    if (write_file(dst, read_file(src))) {
        result.imported.push_back("AGENTS.md");
        ++result.item_count;
    } else {
        result.errors.push_back("AGENTS.md: write failed");
    }
}

// ── Orchestrator ────────────────────────────────────────────────────────

MigrationResult migrate(const MigrateOptions& opts) {
    MigrationResult r;
    migrate_soul(opts, r);
    migrate_memories(opts, r);
    migrate_skills(opts, r);
    migrate_command_allowlist(opts, r);
    migrate_messaging(opts, r);
    if (preset_includes_secrets(opts.preset)) {
        migrate_api_keys(opts, r);
    } else {
        r.skipped.push_back("api keys (preset=" + opts.preset + ")");
    }
    if (preset_includes_workspace(opts.preset)) {
        migrate_tts_assets(opts, r);
    } else {
        r.skipped.push_back("tts assets (preset=" + opts.preset + ")");
    }
    migrate_agents_md(opts, r);
    return r;
}

}  // namespace hermes::cli::claw
