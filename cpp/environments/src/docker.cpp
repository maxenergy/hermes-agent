#include "hermes/environments/docker.hpp"
#include "hermes/environments/env_filter.hpp"

#include <algorithm>
#include <cctype>
#include <sstream>
#include <string_view>

namespace hermes::environments {

namespace {

// Shell-quote a single argument: wrap in single quotes, escape embedded
// single quotes.  Good enough for Linux/macOS /bin/sh contexts; the
// rest of the file already uses the same trick.
std::string sq(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 2);
    out.push_back('\'');
    for (char c : s) {
        if (c == '\'') {
            out += "'\\''";
        } else {
            out.push_back(c);
        }
    }
    out.push_back('\'');
    return out;
}

// Return true if `image` already has a `@sha256:...` digest suffix.
bool has_digest(std::string_view image) {
    auto at = image.find('@');
    if (at == std::string_view::npos) return false;
    auto rest = image.substr(at + 1);
    // Accept @sha256:<64 hex> (or any digest-algo:hex).
    auto colon = rest.find(':');
    return colon != std::string_view::npos && colon + 1 < rest.size();
}

// Split `image` into repository (everything before the first ':' after
// the last '/') and a tag / digest suffix.  Returns (repo, tag_or_digest).
// When no tag is present, tag_or_digest is empty and repo equals input.
std::pair<std::string, std::string> split_tag(std::string_view image) {
    // Locate the tag separator — the last ':' that appears after the
    // last '/'.  `registry:5000/repo:tag` needs the `:tag` split, not
    // the registry port.
    auto last_slash = image.rfind('/');
    auto scan_from = last_slash == std::string_view::npos ? 0 : last_slash + 1;
    auto colon = image.find(':', scan_from);
    if (colon == std::string_view::npos) {
        return {std::string(image), ""};
    }
    return {std::string(image.substr(0, colon)),
            std::string(image.substr(colon + 1))};
}

// Extract the "Digest" field out of `docker manifest inspect --verbose`
// JSON output without linking to a JSON library.  The output typically
// contains a top-level `"Descriptor":{ "digest":"sha256:..."}` record
// and may be a JSON array (one object per platform) — in that case we
// return the first digest.  Also accepts the simpler non-verbose output
// which is already a single SHA on stdout.
std::string extract_digest(const std::string& json) {
    // Trivial case — non-verbose `docker manifest inspect` prints a
    // single-line JSON for a v2 manifest whose `.config.digest` is the
    // image digest.  But the verbose form is more portable: look for
    // the first `"digest"` key.
    std::string key = "\"digest\"";
    auto pos = json.find(key);
    while (pos != std::string::npos) {
        auto colon = json.find(':', pos + key.size());
        if (colon == std::string::npos) break;
        auto quote = json.find('"', colon);
        if (quote == std::string::npos) break;
        auto end = json.find('"', quote + 1);
        if (end == std::string::npos) break;
        auto value = json.substr(quote + 1, end - quote - 1);
        // Accept only algo:hex forms.
        if (value.find(':') != std::string::npos) {
            return value;
        }
        pos = json.find(key, end);
    }
    // Fallback: trimmed stdout could itself be the digest.
    std::string trimmed = json;
    auto not_ws = [](char c) { return !std::isspace(static_cast<unsigned char>(c)); };
    auto lit = std::find_if(trimmed.begin(), trimmed.end(), not_ws);
    trimmed.erase(trimmed.begin(), lit);
    auto rit = std::find_if(trimmed.rbegin(), trimmed.rend(), not_ws);
    trimmed.erase(rit.base(), trimmed.end());
    if (trimmed.rfind("sha256:", 0) == 0) return trimmed;
    return {};
}

}  // namespace

DockerEnvironment::DockerEnvironment() : config_() {}

DockerEnvironment::DockerEnvironment(Config config)
    : config_(std::move(config)) {}

DockerEnvironment::~DockerEnvironment() = default;

std::vector<std::string> DockerEnvironment::build_docker_args(
    const ExecuteOptions& opts) const {
    std::vector<std::string> args;

    args.emplace_back("run");
    args.emplace_back("--rm");
    args.emplace_back("-i");

    // Security flags.
    if (config_.cap_drop_all) {
        args.emplace_back("--cap-drop=ALL");
    }
    if (config_.no_new_privileges) {
        args.emplace_back("--security-opt");
        args.emplace_back("no-new-privileges");
    }

    // Resource limits.
    if (config_.pids_limit) {
        args.emplace_back("--pids-limit");
        args.emplace_back(std::to_string(*config_.pids_limit));
    }
    if (config_.cpus) {
        args.emplace_back("--cpus");
        // Format with reasonable precision.
        std::ostringstream oss;
        oss << *config_.cpus;
        args.emplace_back(oss.str());
    }
    if (config_.memory) {
        args.emplace_back("--memory");
        args.emplace_back(*config_.memory);
    }

    // Mounts.
    for (const auto& mount : config_.bind_mounts) {
        args.emplace_back("-v");
        args.emplace_back(mount);
    }
    for (const auto& tmpfs : config_.tmpfs_mounts) {
        args.emplace_back("--tmpfs");
        args.emplace_back(tmpfs);
    }

    // Working directory.
    if (!opts.cwd.empty()) {
        args.emplace_back("-w");
        args.emplace_back(opts.cwd.string());
    }

    // Filtered environment variables.
    auto filtered = filter_env(opts.env_vars);
    for (const auto& [k, v] : filtered) {
        args.emplace_back("-e");
        args.emplace_back(k + "=" + v);
    }

    return args;
}

CompletedProcess DockerEnvironment::execute(const std::string& cmd,
                                            const ExecuteOptions& opts) {
    auto docker_args = build_docker_args(opts);

    // Build the full command line.
    std::ostringstream full_cmd;
    full_cmd << "docker";
    for (const auto& arg : docker_args) {
        full_cmd << " '" << arg << "'";
    }
    full_cmd << " '" << config_.image << "'";
    full_cmd << " bash -c '" << cmd << "'";

    ExecuteOptions local_opts;
    local_opts.timeout = opts.timeout;
    local_opts.cancel_fn = opts.cancel_fn;
    // Don't pass env or cwd — those are in the docker args.

    return local_.execute(full_cmd.str(), local_opts);
}

void DockerEnvironment::cleanup() {
    // If we had a persistent container, stop it here.
    // For --rm containers, nothing to do.

    // Prune any explicitly-tracked anonymous volumes.  Ignore errors —
    // cleanup is best-effort.
    for (const auto& vol : anonymous_volumes_) {
        (void)run_docker({"volume", "rm", "-f", vol});
    }
    anonymous_volumes_.clear();
}

CompletedProcess DockerEnvironment::run_docker(
    const std::vector<std::string>& argv) {
    if (runner_) return runner_(argv);

    std::ostringstream full_cmd;
    full_cmd << "docker";
    for (const auto& a : argv) {
        full_cmd << ' ' << sq(a);
    }
    ExecuteOptions opts;
    opts.timeout = std::chrono::seconds{60};
    return local_.execute(full_cmd.str(), opts);
}

std::string DockerEnvironment::resolve_image_digest(const std::string& image) {
    // Already digest-pinned — nothing to do.
    if (has_digest(image)) return image;

    // Cache check.
    auto it = digest_cache_.find(image);
    if (it != digest_cache_.end()) return it->second;

    auto [repo, tag] = split_tag(image);

    // Call `docker manifest inspect --verbose <image>`.  This requires
    // experimental CLI (default since 20.10) and may fail for offline
    // runs — caller decides what to do.
    auto result = run_docker({"manifest", "inspect", "--verbose", image});
    if (result.exit_code != 0) {
        // Fallback: try `docker buildx imagetools inspect` format which
        // is sometimes available even when `manifest` is not.
        result = run_docker({"buildx", "imagetools", "inspect", image});
        if (result.exit_code != 0) return {};
    }
    std::string digest = extract_digest(result.stdout_text);
    if (digest.empty()) return {};

    std::string pinned = repo + "@" + digest;
    digest_cache_[image] = pinned;
    return pinned;
}

bool DockerEnvironment::pin_configured_image() {
    auto pinned = resolve_image_digest(config_.image);
    if (pinned.empty()) return false;
    config_.image = pinned;
    return true;
}

void DockerEnvironment::track_anonymous_volume(std::string volume_id) {
    if (volume_id.empty()) return;
    anonymous_volumes_.push_back(std::move(volume_id));
}

}  // namespace hermes::environments
