// Phase 12 — Per-platform channel/contact cache.
//
// Persists a small JSON blob under {HERMES_HOME}/cache/<platform>/channels.json
// so adapters can avoid round-trips when resolving channel_id → name (and
// vice versa).  Updated opportunistically as the gateway sees messages.
#pragma once

#include <filesystem>
#include <map>
#include <optional>
#include <string>

namespace hermes::gateway {

struct ChannelEntry {
    std::string id;            // platform-native id (chat_id, channel_id, jid)
    std::string name;          // human-readable display name
    std::string kind;          // "dm" | "group" | "channel" | "thread"
    std::map<std::string, std::string> extras;  // platform-specific keys
};

class ChannelCache {
public:
    // Construct with the platform short name (e.g. "telegram", "slack",
    // "discord").  An optional root_override is used by tests.
    explicit ChannelCache(std::string platform,
                          std::filesystem::path root_override = {});

    // Load from disk (no-op when the file does not exist).
    void load();

    // Persist current state to disk atomically.
    void save() const;

    // Insert or update an entry (in-memory only; call save() to persist).
    void upsert(const ChannelEntry& entry);

    // Lookups.
    std::optional<ChannelEntry> by_id(const std::string& id) const;
    std::optional<ChannelEntry> by_name(const std::string& name) const;

    // Bulk accessor — copy of current map.
    std::map<std::string, ChannelEntry> entries() const { return by_id_; }

    // Path to the on-disk JSON file.
    std::filesystem::path file_path() const;

private:
    std::string platform_;
    std::filesystem::path root_override_;
    std::map<std::string, ChannelEntry> by_id_;
};

}  // namespace hermes::gateway
