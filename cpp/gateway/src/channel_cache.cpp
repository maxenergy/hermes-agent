// Phase 12 — Per-platform channel cache implementation.
#include <hermes/gateway/channel_cache.hpp>

#include <fstream>

#include <nlohmann/json.hpp>

#include <hermes/core/path.hpp>

namespace hermes::gateway {

namespace fs = std::filesystem;

ChannelCache::ChannelCache(std::string platform, fs::path root_override)
    : platform_(std::move(platform)),
      root_override_(std::move(root_override)) {}

fs::path ChannelCache::file_path() const {
    fs::path root = root_override_.empty()
                        ? hermes::core::path::get_hermes_home() / "cache"
                        : root_override_;
    return root / platform_ / "channels.json";
}

void ChannelCache::load() {
    auto path = file_path();
    if (!fs::exists(path)) return;
    std::ifstream in(path);
    if (!in) return;
    try {
        nlohmann::json j;
        in >> j;
        if (!j.is_object()) return;
        for (auto& [id, raw] : j.items()) {
            ChannelEntry e;
            e.id = id;
            e.name = raw.value("name", "");
            e.kind = raw.value("kind", "");
            if (raw.contains("extras") && raw["extras"].is_object()) {
                for (auto& [k, v] : raw["extras"].items()) {
                    if (v.is_string()) e.extras[k] = v.get<std::string>();
                }
            }
            by_id_[id] = std::move(e);
        }
    } catch (...) {
        // Corrupt cache — start fresh.
        by_id_.clear();
    }
}

void ChannelCache::save() const {
    auto path = file_path();
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);

    nlohmann::json j = nlohmann::json::object();
    for (const auto& [id, e] : by_id_) {
        nlohmann::json o;
        o["name"] = e.name;
        o["kind"] = e.kind;
        if (!e.extras.empty()) {
            nlohmann::json ex = nlohmann::json::object();
            for (const auto& [k, v] : e.extras) ex[k] = v;
            o["extras"] = std::move(ex);
        }
        j[id] = std::move(o);
    }

    auto tmp = path;
    tmp += ".tmp";
    {
        std::ofstream out(tmp);
        out << j.dump(2);
    }
    fs::rename(tmp, path, ec);
    if (ec) fs::remove(tmp);
}

void ChannelCache::upsert(const ChannelEntry& entry) {
    if (entry.id.empty()) return;
    by_id_[entry.id] = entry;
}

std::optional<ChannelEntry> ChannelCache::by_id(const std::string& id) const {
    auto it = by_id_.find(id);
    if (it == by_id_.end()) return std::nullopt;
    return it->second;
}

std::optional<ChannelEntry> ChannelCache::by_name(
    const std::string& name) const {
    for (const auto& [_, e] : by_id_) {
        if (e.name == name) return e;
    }
    return std::nullopt;
}

}  // namespace hermes::gateway
