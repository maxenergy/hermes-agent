// Phase 12 — Media cache for inbound platform attachments.
//
// Downloads media URLs (images, audio, documents) into a content-addressed
// on-disk cache under {HERMES_HOME}/cache/{kind}/. Implements 429/5xx retry
// with jittered exponential backoff and SSRF protection on the requested
// URL. Used by Telegram, Discord, and Slack file handlers so tool handlers
// receive a local filesystem path instead of a remote URL.
#pragma once

#include <chrono>
#include <filesystem>
#include <optional>
#include <string>
#include <unordered_map>

#include <hermes/llm/llm_client.hpp>

namespace hermes::gateway {

enum class MediaKind {
    Image,     // {HERMES_HOME}/cache/images/
    Audio,     // {HERMES_HOME}/cache/audio/
    Video,     // {HERMES_HOME}/cache/video/
    Document,  // {HERMES_HOME}/cache/documents/
};

struct MediaCacheResult {
    bool ok = false;
    std::filesystem::path path;     // local cache path (when ok==true)
    int status_code = 0;            // last HTTP status from upstream
    std::string error;              // human-readable error (when !ok)
    int attempts = 0;               // total attempts including retries
    bool from_cache = false;        // true when served from existing cache
};

class MediaCache {
public:
    struct Options {
        // Maximum number of HTTP attempts (including the first). 4 attempts
        // ≈ 1s + 2s + 4s back-off windows.
        int max_attempts = 4;
        std::chrono::milliseconds base_delay = std::chrono::seconds(1);
        std::chrono::milliseconds max_delay = std::chrono::seconds(30);
        // Reject URLs whose host resolves to a private/loopback/metadata IP.
        bool ssrf_protection = true;
        // Custom root override (defaults to {HERMES_HOME}/cache).
        std::filesystem::path root_override;
    };

    MediaCache();
    explicit MediaCache(Options opts);
    // Test-friendly constructor with an injected HTTP transport.
    MediaCache(Options opts, hermes::llm::HttpTransport* transport);

    // Resolve a URL to a local cache path. Re-uses existing on-disk file
    // when the same URL has been fetched before (content-addressed by URL
    // sha256). Performs retry with exponential backoff on 429/5xx.
    MediaCacheResult fetch(const std::string& url, MediaKind kind,
                           const std::unordered_map<std::string, std::string>&
                               headers = {});

    // Resolve the cache path for a URL without performing IO. Useful for
    // checking whether a media item is already on disk.
    std::filesystem::path cache_path_for(const std::string& url,
                                         MediaKind kind) const;

    // Root cache directory (e.g., {HERMES_HOME}/cache).
    std::filesystem::path root() const;

    // Subdirectory for a given kind.
    std::filesystem::path dir_for(MediaKind kind) const;

    // Internal helper: classify an HTTP status as retryable.
    static bool is_retryable_status(int status_code);

private:
    hermes::llm::HttpTransport* get_transport();

    Options opts_;
    hermes::llm::HttpTransport* transport_ = nullptr;
};

}  // namespace hermes::gateway
