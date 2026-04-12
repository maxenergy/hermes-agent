#include <hermes/gateway/sticker_cache.hpp>

#include <fstream>

namespace hermes::gateway {

namespace fs = std::filesystem;

StickerCache::StickerCache(fs::path cache_dir) : dir_(std::move(cache_dir)) {
    std::error_code ec;
    fs::create_directories(dir_, ec);
}

void StickerCache::store(const std::string& sticker_id,
                         const std::string& data) {
    auto path = dir_ / sticker_id;
    std::ofstream ofs(path, std::ios::binary);
    if (ofs.is_open()) {
        ofs.write(data.data(), static_cast<std::streamsize>(data.size()));
    }
}

std::optional<std::string> StickerCache::get(
    const std::string& sticker_id) const {
    auto path = dir_ / sticker_id;
    if (!fs::exists(path)) return std::nullopt;

    std::ifstream ifs(path, std::ios::binary);
    if (!ifs.is_open()) return std::nullopt;

    std::string content((std::istreambuf_iterator<char>(ifs)),
                        std::istreambuf_iterator<char>());
    return content;
}

void StickerCache::cleanup_older_than(std::chrono::hours age) {
    std::error_code ec;
    auto now = fs::file_time_type::clock::now();

    for (auto it = fs::directory_iterator(dir_, ec); it != fs::directory_iterator(); ++it) {
        if (!it->is_regular_file()) continue;
        auto ftime = it->last_write_time(ec);
        if (ec) continue;

        auto file_age = std::chrono::duration_cast<std::chrono::hours>(
            now - ftime);
        if (file_age > age) {
            fs::remove(it->path(), ec);
        }
    }
}

}  // namespace hermes::gateway
