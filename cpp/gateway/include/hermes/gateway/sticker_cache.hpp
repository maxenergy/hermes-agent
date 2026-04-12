// StickerCache — filesystem-backed cache for sticker data.
#pragma once

#include <chrono>
#include <filesystem>
#include <optional>
#include <string>

namespace hermes::gateway {

class StickerCache {
public:
    explicit StickerCache(std::filesystem::path cache_dir);

    void store(const std::string& sticker_id, const std::string& data);
    std::optional<std::string> get(const std::string& sticker_id) const;
    void cleanup_older_than(std::chrono::hours age);

private:
    std::filesystem::path dir_;
};

}  // namespace hermes::gateway
