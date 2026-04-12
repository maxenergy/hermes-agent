// ChannelDirectory — registry of known channels across platforms.
#pragma once

#include <hermes/gateway/gateway_config.hpp>

#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace hermes::gateway {

class ChannelDirectory {
public:
    struct ChannelInfo {
        std::string id;
        std::string name;
        Platform platform;
    };

    void register_channel(ChannelInfo info);
    std::optional<ChannelInfo> resolve_by_name(const std::string& name) const;
    std::vector<ChannelInfo> list_all() const;
    void clear();

private:
    std::vector<ChannelInfo> channels_;
    mutable std::mutex mu_;
};

}  // namespace hermes::gateway
