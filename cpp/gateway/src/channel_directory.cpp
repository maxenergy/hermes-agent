#include <hermes/gateway/channel_directory.hpp>

namespace hermes::gateway {

void ChannelDirectory::register_channel(ChannelInfo info) {
    std::lock_guard<std::mutex> lock(mu_);
    // Avoid duplicates by id+platform.
    for (auto& ch : channels_) {
        if (ch.id == info.id && ch.platform == info.platform) {
            ch.name = info.name;  // Update name if re-registered.
            return;
        }
    }
    channels_.push_back(std::move(info));
}

std::optional<ChannelDirectory::ChannelInfo>
ChannelDirectory::resolve_by_name(const std::string& name) const {
    std::lock_guard<std::mutex> lock(mu_);
    for (const auto& ch : channels_) {
        if (ch.name == name) return ch;
    }
    return std::nullopt;
}

std::vector<ChannelDirectory::ChannelInfo>
ChannelDirectory::list_all() const {
    std::lock_guard<std::mutex> lock(mu_);
    return channels_;
}

void ChannelDirectory::clear() {
    std::lock_guard<std::mutex> lock(mu_);
    channels_.clear();
}

}  // namespace hermes::gateway
