// Phase 12 — Media cache implementation.
#include <hermes/gateway/media_cache.hpp>

#include <algorithm>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <thread>

#include <openssl/sha.h>

#include <hermes/core/path.hpp>
#include <hermes/core/retry.hpp>
#include <hermes/core/url_safety.hpp>

namespace hermes::gateway {

namespace fs = std::filesystem;

namespace {

std::string sha256_hex(const std::string& s) {
    unsigned char hash[SHA256_DIGEST_LENGTH];
    SHA256(reinterpret_cast<const unsigned char*>(s.data()), s.size(), hash);
    std::ostringstream oss;
    for (unsigned char b : hash) {
        oss << std::hex << std::setfill('0') << std::setw(2)
            << static_cast<int>(b);
    }
    return oss.str();
}

// Best-effort filename suffix from a URL.  Returns the trailing extension
// (with leading dot) when the path component has one, else "".
std::string url_suffix(const std::string& url) {
    auto qpos = url.find_first_of("?#");
    auto path = (qpos == std::string::npos) ? url : url.substr(0, qpos);
    auto slash = path.find_last_of('/');
    if (slash == std::string::npos) return "";
    auto fname = path.substr(slash + 1);
    auto dot = fname.find_last_of('.');
    if (dot == std::string::npos) return "";
    auto ext = fname.substr(dot);
    // Sanitise — only allow short alphanumeric extensions.
    if (ext.size() > 8) return "";
    for (size_t i = 1; i < ext.size(); ++i) {
        char c = ext[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9'))) {
            return "";
        }
    }
    return ext;
}

const char* kind_dirname(MediaKind k) {
    switch (k) {
        case MediaKind::Image:    return "images";
        case MediaKind::Audio:    return "audio";
        case MediaKind::Video:    return "video";
        case MediaKind::Document: return "documents";
    }
    return "other";
}

}  // namespace

MediaCache::MediaCache() = default;
MediaCache::MediaCache(Options opts) : opts_(std::move(opts)) {}

MediaCache::MediaCache(Options opts, hermes::llm::HttpTransport* transport)
    : opts_(std::move(opts)), transport_(transport) {}

hermes::llm::HttpTransport* MediaCache::get_transport() {
    if (transport_) return transport_;
    return hermes::llm::get_default_transport();
}

fs::path MediaCache::root() const {
    if (!opts_.root_override.empty()) return opts_.root_override;
    return hermes::core::path::get_hermes_home() / "cache";
}

fs::path MediaCache::dir_for(MediaKind kind) const {
    return root() / kind_dirname(kind);
}

fs::path MediaCache::cache_path_for(const std::string& url,
                                    MediaKind kind) const {
    auto suffix = url_suffix(url);
    auto name = sha256_hex(url) + suffix;
    return dir_for(kind) / name;
}

bool MediaCache::is_retryable_status(int code) {
    return code == 408 || code == 429 || (code >= 500 && code <= 599);
}

MediaCacheResult MediaCache::fetch(
    const std::string& url, MediaKind kind,
    const std::unordered_map<std::string, std::string>& headers) {
    MediaCacheResult result;
    result.path = cache_path_for(url, kind);

    std::error_code ec;
    fs::create_directories(result.path.parent_path(), ec);

    // Cache hit — return immediately.
    if (fs::exists(result.path, ec) && fs::file_size(result.path, ec) > 0) {
        result.ok = true;
        result.from_cache = true;
        return result;
    }

    if (opts_.ssrf_protection &&
        !hermes::core::url_safety::is_safe_url(url)) {
        result.error = "ssrf_blocked: refusing to fetch private/loopback URL";
        return result;
    }

    auto* transport = get_transport();
    if (!transport) {
        result.error = "no http transport available";
        return result;
    }

    for (int attempt = 1; attempt <= std::max(1, opts_.max_attempts);
         ++attempt) {
        result.attempts = attempt;
        try {
            auto resp = transport->get(url, headers);
            result.status_code = resp.status_code;
            if (resp.status_code >= 200 && resp.status_code < 300) {
                // Atomic write: temp file + rename.
                auto tmp = result.path;
                tmp += ".part";
                {
                    std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
                    if (!out) {
                        result.error = "failed to open temp file: " +
                                       tmp.string();
                        fs::remove(tmp, ec);
                        return result;
                    }
                    out.write(resp.body.data(),
                              static_cast<std::streamsize>(resp.body.size()));
                }
                fs::rename(tmp, result.path, ec);
                if (ec) {
                    result.error = "rename failed: " + ec.message();
                    fs::remove(tmp);
                    return result;
                }
                result.ok = true;
                return result;
            }

            if (!is_retryable_status(resp.status_code) ||
                attempt == opts_.max_attempts) {
                result.error = "http " +
                               std::to_string(resp.status_code) + " (final)";
                return result;
            }
        } catch (const std::exception& e) {
            result.error = std::string("transport exception: ") + e.what();
            // Treat exceptions as retryable up to max_attempts.
            if (attempt == opts_.max_attempts) return result;
        } catch (...) {
            result.error = "unknown transport exception";
            if (attempt == opts_.max_attempts) return result;
        }

        auto delay = hermes::core::retry::jittered_backoff(
            attempt, opts_.base_delay, opts_.max_delay);
        std::this_thread::sleep_for(delay);
    }

    return result;
}

}  // namespace hermes::gateway
